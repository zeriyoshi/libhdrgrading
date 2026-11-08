/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <string.h>
#include <unity.h>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "../test_util.h"
#include "hdrgrading/video.h"

/*
 * The GStreamer plugin: appsrc → hdrgrade → appsink. Every
 * frame out equals the session's output for the same frames, with a future
 * window (held, then sent on at EOS), the buffers keep their timestamps, the
 * caps carry PQ over BT.2020 and the mastering display, the latency query
 * adds the window's future; R'G'B' input too, the allocation query asking
 * upstream for the frames it holds besides. With out-rate (twice the
 * input's), 2 n - 1 frames, the frames between the session's at that rate,
 * the source pad's rate and the timestamps the output rate's. Each buffer's
 * HdrgDynamicHdrMeta holds the session's T.35 payloads of the frame.
 */
#define TG_W      64
#define TG_H      48
#define TG_FRAMES 20
#define TG_RATE   30

/* the window (none in a build without the temporal filters) */
#if HDRG_BUILTIN_STATS
#define TG_PAST   4
#define TG_FUTURE 2
#else
#define TG_PAST   0
#define TG_FUTURE 0
#endif

#define TG_OUT_MAX (2 * TG_FRAMES) /* the reference's outputs at most */

static uint16_t *g_reference[TG_OUT_MAX][3];
static uint8_t g_plus[TG_OUT_MAX][HDRG_HDR10PLUS_T35_BYTES]; /* ... their T.35 payloads */
static uint8_t g_curve[TG_OUT_MAX][HDRG_ST2094_50_T35_MAX];
static size_t g_plus_size[TG_OUT_MAX], g_curve_size[TG_OUT_MAX];
static uint32_t g_out_rate; /* the element's out-rate (0: the input's) */
static int32_t g_count;     /* the reference's outputs */

/* frame t of format (I420, NV12 or RGB) laid out as GStreamer's default for it */
static GstBuffer *tg_frame(GstVideoFormat format, int32_t t)
{
    GstVideoFrame frame;
    GstVideoInfo info;
    GstBuffer *buffer;
    uint8_t *row;
    int32_t x, y, p, w, h;

    gst_video_info_set_format(&info, format, TG_W, TG_H);
    buffer = gst_buffer_new_allocate(NULL, GST_VIDEO_INFO_SIZE(&info), NULL);
    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_TRUE(gst_video_frame_map(&frame, &info, buffer, GST_MAP_WRITE));

    for (p = 0; p < (int32_t)GST_VIDEO_FRAME_N_PLANES(&frame); p++) {
        w = GST_VIDEO_FRAME_COMP_WIDTH(&frame, p) * GST_VIDEO_FRAME_COMP_PSTRIDE(&frame, p);
        h = GST_VIDEO_FRAME_COMP_HEIGHT(&frame, p);

        for (y = 0; y < h; y++) {
            row = (uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&frame, p) +
                  y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, p);

            for (x = 0; x < w; x++) {
                row[x] = (uint8_t)((p == 0) ? 16 + (x * 3 + y * 2 + t * 5) % 200
                                            : 112 + (x + y + t) % 32);
            }
        }
    }

    gst_video_frame_unmap(&frame);
    GST_BUFFER_PTS(buffer) = gst_util_uint64_scale((guint64)t, GST_SECOND, TG_RATE);
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, TG_RATE);

    return buffer;
}

/*
 * The session's output for the frames of format, as the element is set up
 * (Y'CbCr: chroma at chroma_loc; the frames kept mapped to the end: R'G'B'
 * is read until it renders)
 */
static void tg_reference(GstVideoFormat format, uint8_t chroma_loc)
{
    hdrg_session_config_t config;
    hdrg_display_caps_t caps;
    hdrg_session_format_t raw;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    GstVideoFrame frames[TG_FRAMES];
    GstVideoInfo info;
    GstBuffer *buffer;
    uint8_t p;
    int32_t sent, received;

    gst_video_info_set_format(&info, format, TG_W, TG_H);
    memset(&raw, 0, sizeof(raw));
    raw.width = TG_W;
    raw.height = TG_H;
    raw.primaries = HDRG_PRIMARIES_BT709;
    hdrg_session_config_init(&config);

    if (format == GST_VIDEO_FORMAT_RGB) {
        raw.transfer = HDRG_TRANSFER_SRGB;
        raw.bit_depth = 8;
        raw.stride = (size_t)GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
    } else {
        raw.transfer = HDRG_TRANSFER_BT709;
        config.input.layout = HDRG_YUV_420;
        config.input.matrix = HDRG_MATRIX_BT709;
        config.input.bits = 8;
        config.input.chroma_loc = chroma_loc;
        config.input.interleaved = format == GST_VIDEO_FORMAT_NV12;
    }

    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;
    config.past = TG_PAST;
    config.future = TG_FUTURE;
    config.output.layout = HDRG_YUV_420;
    config.output.matrix = HDRG_MATRIX_BT2020;
    config.output.bits = 10;
    config.rate_num = TG_RATE;
    config.rate_den = 1;
    config.out_rate_num = g_out_rate;
    config.out_rate_den = (g_out_rate != 0) ? 1u : 0u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, &raw, NULL, &caps, &config, &session));

    sent = 0;
    received = 0;

    for (;;) {
        TEST_ASSERT_TRUE(received < TG_OUT_MAX);
        memset(&out, 0, sizeof(out));

        for (p = 0; p < 3u; p++) {
            if (g_reference[received][p] == NULL) {
                g_reference[received][p] = (uint16_t *)calloc((size_t)(TG_W * TG_H), 2);
            }

            out.planes[p] = (uint8_t *)g_reference[received][p];
            out.strides[p] = (size_t)((p == 0) ? TG_W : TG_W / 2) * 2u;
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            buffer = (sent < TG_FRAMES) ? tg_frame(format, sent) : NULL;

            if (buffer != NULL) {
                TEST_ASSERT_TRUE(gst_video_frame_map(&frames[sent], &info, buffer, GST_MAP_READ));
                gst_buffer_unref(buffer);

                for (p = 0; p < GST_VIDEO_FRAME_N_PLANES(&frames[sent]); p++) {
                    in.planes[p] = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&frames[sent], p);
                    in.strides[p] = (size_t)GST_VIDEO_FRAME_PLANE_STRIDE(&frames[sent], p);
                }

                in.pts = sent;
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, buffer ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        g_plus_size[received] =
            hdrg_hdr10plus_t35(&out.hdr10plus, g_plus[received], sizeof(g_plus[received]));
        g_curve_size[received] =
            hdrg_st2094_50_t35(&out.st2094_50, g_curve[received], sizeof(g_curve[received]));
        received++;
    }

    g_count = received;

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    for (sent = 0; sent < TG_FRAMES; sent++) {
        gst_video_frame_unmap(&frames[sent]);
    }
}

/* buffer's HdrgDynamicHdrMeta: the field's bytes those of want (size 0: no field) */
static void tg_dynamic(GstBuffer *buffer, const char *field, const uint8_t *want, size_t size)
{
    const GValue *value;
    GstCustomMeta *meta;
    GstBuffer *bytes;
    GstMapInfo map;

    meta = gst_buffer_get_custom_meta(buffer, "HdrgDynamicHdrMeta");
    value =
        (meta != NULL) ? gst_structure_get_value(gst_custom_meta_get_structure(meta), field) : NULL;

    if (size == 0) {
        TEST_ASSERT_NULL(value);

        return;
    }

    TEST_ASSERT_NOT_NULL(value);
    TEST_ASSERT_TRUE(GST_VALUE_HOLDS_BUFFER(value));
    bytes = gst_value_get_buffer(value);
    TEST_ASSERT_TRUE(gst_buffer_map(bytes, &map, GST_MAP_READ));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)size, (uint32_t)map.size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, map.data, size);
    gst_buffer_unmap(bytes, &map);
}

/* the pipeline over the frames of format (chroma_site NULL: none in the caps), outputs checked */
static void tg_run(GstVideoFormat format, const char *colorimetry, const char *chroma_site)
{
    const uint16_t *row;
    GstElement *pipeline, *src, *grade, *sink;
    GstVideoMasteringDisplayInfo master;
    GstVideoFrame frame;
    GstVideoInfo info;
    GstSample *sample;
    GstBuffer *buffer;
    GstQuery *query;
    GstCaps *caps;
    GstPad *pad;
    GstClockTime min, max, expected;
    gboolean live;
    guint size, held;
    int32_t t, p, x, y, w, h;
    char rate[32];

    pipeline = gst_pipeline_new(NULL);
    src = gst_element_factory_make("appsrc", NULL);
    grade = gst_element_factory_make("hdrgrade", NULL);
    sink = gst_element_factory_make("appsink", NULL);
    TEST_ASSERT_TRUE(pipeline != NULL && src != NULL && grade != NULL && sink != NULL);

    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                               gst_video_format_to_string(format), "width", G_TYPE_INT, TG_W,
                               "height", G_TYPE_INT, TG_H, "framerate", GST_TYPE_FRACTION, TG_RATE,
                               1, "colorimetry", G_TYPE_STRING, colorimetry, NULL);

    if (chroma_site != NULL) {
        gst_caps_set_simple(caps, "chroma-site", G_TYPE_STRING, chroma_site, NULL);
    }

    g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, NULL);
    gst_caps_unref(caps);
    g_object_set(grade, "past", (guint)TG_PAST, "future", (guint)TG_FUTURE, NULL);

    if (g_out_rate != 0) {
        snprintf(rate, sizeof(rate), "%u/1", g_out_rate);
        gst_util_set_object_arg(G_OBJECT(grade), "out-rate", rate);
    }

    g_object_set(sink, "sync", FALSE, NULL);
    gst_bin_add_many(GST_BIN(pipeline), src, grade, sink, NULL);
    TEST_ASSERT_TRUE(gst_element_link_many(src, grade, sink, NULL));
    TEST_ASSERT_TRUE(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
                     GST_STATE_CHANGE_FAILURE);

    for (t = 0; t < TG_FRAMES; t++) {
        TEST_ASSERT_EQUAL_INT(GST_FLOW_OK,
                              gst_app_src_push_buffer(GST_APP_SRC(src), tg_frame(format, t)));
    }

    TEST_ASSERT_EQUAL_INT(GST_FLOW_OK, gst_app_src_end_of_stream(GST_APP_SRC(src)));

    for (t = 0; t < g_count; t++) {
        sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        TEST_ASSERT_NOT_NULL(sample);
        caps = gst_sample_get_caps(sample);
        buffer = gst_sample_get_buffer(sample);
        TEST_ASSERT_TRUE(gst_video_info_from_caps(&info, caps));
        TEST_ASSERT_EQUAL_INT(GST_VIDEO_FORMAT_I420_10LE, GST_VIDEO_INFO_FORMAT(&info));
        TEST_ASSERT_EQUAL_INT(GST_VIDEO_TRANSFER_SMPTE2084, info.colorimetry.transfer);
        TEST_ASSERT_EQUAL_INT(GST_VIDEO_COLOR_PRIMARIES_BT2020, info.colorimetry.primaries);
        TEST_ASSERT_EQUAL_INT(GST_VIDEO_COLOR_MATRIX_BT2020, info.colorimetry.matrix);
        TEST_ASSERT_TRUE(gst_video_mastering_display_info_from_caps(&master, caps));
        TEST_ASSERT_EQUAL_UINT32(10000000, master.max_display_mastering_luminance);
        expected =
            gst_util_uint64_scale((guint64)t, GST_SECOND, (g_out_rate != 0) ? g_out_rate : TG_RATE);
        TEST_ASSERT_TRUE(GST_BUFFER_PTS(buffer) + 1u >= expected &&
                         GST_BUFFER_PTS(buffer) <= expected + 1u);
        TEST_ASSERT_EQUAL_INT((g_out_rate != 0) ? (int32_t)g_out_rate : TG_RATE,
                              GST_VIDEO_INFO_FPS_N(&info));
        TEST_ASSERT_TRUE(gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ));

        for (p = 0; p < 3; p++) {
            w = (p == 0) ? TG_W : TG_W / 2;
            h = (p == 0) ? TG_H : TG_H / 2;

            for (y = 0; y < h; y++) {
                row = (const uint16_t *)(const void *)((const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(
                                                           &frame, p) +
                                                       y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, p));

                for (x = 0; x < w; x++) {
                    if (row[x] != g_reference[t][p][y * w + x]) {
                        TEST_FAIL_MESSAGE("a frame differs from the session's");
                    }
                }
            }
        }

        gst_video_frame_unmap(&frame);
        tg_dynamic(buffer, "hdr10plus", g_plus[t], g_plus_size[t]);
        tg_dynamic(buffer, "st2094-50", g_curve[t], g_curve_size[t]);
        gst_sample_unref(sample);
    }

    TEST_ASSERT_TRUE(gst_app_sink_is_eos(GST_APP_SINK(sink)) ||
                     gst_app_sink_pull_sample(GST_APP_SINK(sink)) == NULL);

    /* the latency: at least the window's future */
    query = gst_query_new_latency();
    pad = gst_element_get_static_pad(grade, "src");
    TEST_ASSERT_TRUE(gst_pad_query(pad, query));
    gst_query_parse_latency(query, &live, &min, &max);
    TEST_ASSERT_TRUE(min >= gst_util_uint64_scale(TG_FUTURE, GST_SECOND, TG_RATE));
    gst_object_unref(pad);
    gst_query_unref(query);

    /* R'G'B' stays mapped until its output: upstream's pool holds the window's future besides */
    pad = gst_element_get_static_pad(grade, "sink");
    caps = gst_pad_get_current_caps(pad);
    TEST_ASSERT_NOT_NULL(caps);
    query = gst_query_new_allocation(caps, TRUE);
    TEST_ASSERT_TRUE(gst_pad_query(pad, query));

    if (format == GST_VIDEO_FORMAT_RGB) {
        TEST_ASSERT_TRUE(gst_query_get_n_allocation_pools(query) > 0);
        gst_query_parse_nth_allocation_pool(query, 0, NULL, &size, &held, NULL);
        TEST_ASSERT_TRUE(size >= (guint)(TG_W * TG_H * 3));
        TEST_ASSERT_TRUE(held >= TG_FUTURE + 1u);
    } else {
        TEST_ASSERT_EQUAL_UINT32(0, gst_query_get_n_allocation_pools(query));
    }

    gst_caps_unref(caps);
    gst_object_unref(pad);
    gst_query_unref(query);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

/*
 * Y'CbCr: mpeg2 reads as LEFT, jpeg as CENTER, cosited as TOPLEFT; without
 * chroma-site, GStreamer's default for the height (jpeg up to 576 lines)
 */
static void test_yuv(void)
{
    tg_reference(GST_VIDEO_FORMAT_I420, HDRG_CHROMA_CENTER);
    tg_run(GST_VIDEO_FORMAT_I420, "bt709", NULL);
    tg_reference(GST_VIDEO_FORMAT_I420, HDRG_CHROMA_LEFT);
    tg_run(GST_VIDEO_FORMAT_I420, "bt709", "mpeg2");
    tg_reference(GST_VIDEO_FORMAT_I420, HDRG_CHROMA_CENTER);
    tg_run(GST_VIDEO_FORMAT_I420, "bt709", "jpeg");
    tg_reference(GST_VIDEO_FORMAT_I420, HDRG_CHROMA_TOPLEFT);
    tg_run(GST_VIDEO_FORMAT_I420, "bt709", "cosited");
    tg_reference(GST_VIDEO_FORMAT_NV12, HDRG_CHROMA_LEFT);
    tg_run(GST_VIDEO_FORMAT_NV12, "bt709", "mpeg2");
}

static void test_rgb(void)
{
    tg_reference(GST_VIDEO_FORMAT_RGB, HDRG_CHROMA_LEFT);
    tg_run(GST_VIDEO_FORMAT_RGB, "sRGB", NULL);
}

/* whether the build interpolates (an out_rate session: hdrg.interp.mc, HDRG_WITH_TEMPORAL) */
static bool tg_interpolates(void)
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

/* out-rate twice the input's: the frames between, the rate and the timestamps */
static void test_out_rate(void)
{
    if (!tg_interpolates()) {
        TEST_IGNORE_MESSAGE("built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)");
    }

    g_out_rate = 2u * TG_RATE;
    tg_reference(GST_VIDEO_FORMAT_I420, HDRG_CHROMA_CENTER);
    TEST_ASSERT_EQUAL_INT(2 * TG_FRAMES - 1, g_count);
    tg_run(GST_VIDEO_FORMAT_I420, "bt709", NULL);
    g_out_rate = 0;
}

/* the settings are properties: the defaults read, a value set and read back */
static void test_properties(void)
{
    GstElement *grade;
    gfloat diffuse;
    guint past;

    grade = gst_element_factory_make("hdrgrade", NULL);
    TEST_ASSERT_NOT_NULL(grade);
    g_object_get(grade, "diffuse-max", &diffuse, "past", &past, NULL);
    TEST_ASSERT_TRUE(diffuse > 0.0f);
    g_object_set(grade, "diffuse-max", 1.5f, "preset", "anime", NULL);
    g_object_get(grade, "diffuse-max", &diffuse, NULL);
    TEST_ASSERT_TRUE(diffuse != 1.5f);
    g_object_set(grade, "diffuse-max", 1.5f, NULL);
    g_object_get(grade, "diffuse-max", &diffuse, NULL);
    TEST_ASSERT_TRUE(diffuse == 1.5f);
    TEST_ASSERT_NULL(g_object_class_find_property(G_OBJECT_GET_CLASS(grade), "quality"));
    gst_object_unref(grade);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    GError *error;
    GstPlugin *plugin;
    uint8_t p;
    int32_t failures, n;

    gst_init(NULL, NULL);
    error = NULL;
    plugin = gst_plugin_load_file(HDRG_TEST_GST_PLUGIN, &error);
    if (plugin == NULL) {
        g_clear_error(&error);

        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_yuv);
    RUN_TEST(test_rgb);
    RUN_TEST(test_out_rate);
    RUN_TEST(test_properties);
    failures = UNITY_END();

    for (n = 0; n < TG_OUT_MAX; n++) {
        for (p = 0; p < 3u; p++) {
            free(g_reference[n][p]);
        }
    }

    gst_object_unref(plugin);

    return failures;
}
