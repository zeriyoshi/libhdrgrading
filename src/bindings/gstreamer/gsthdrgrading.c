/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * The GStreamer plugin: the element hdrgrade grades SDR
 * video (8 to 16-bit planar YUV, its chroma in a plane each or interleaved
 * as NV12 / P010, packed RGB) into single-layer PQ, I420_10LE
 * of bt2100-pq with the mastering display in the caps, through a session.
 * It uses the public headers only. The window's future is latency: the
 * element holds that many frames, reports them in the latency query and
 * sends them on at EOS. A flush starts the session again. R'G'B' input stays
 * mapped until its output: the allocation query asks upstream for the
 * session's latency and one frame more. The outputs are the source pad's
 * pool's buffers, one taken when a receive may give a frame and kept until
 * one does. out-rate (a
 * fraction, 0/1: the input's) gives the source pad that frame rate, the
 * frames between the input's interpolated by the session, their
 * timestamps its outputs' and their flags the nearer input buffer's. A
 * buffer whose light was measured carries the custom meta
 * HdrgDynamicHdrMeta (GStreamer 1.20): GstBuffer fields hdr10plus and
 * st2094-50, the frame's T.35 payloads of hdrg_hdr10plus_t35 and
 * hdrg_st2094_50_t35 (FFmpeg's), each when measured.
 */
#include <stdint.h>
#include <string.h>

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/version.h"
#include "hdrgrading/video.h"

#define HDRG_GST(obj)      ((hdrg_gst_t *)(obj))
#define HDRG_GST_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define HDRG_GST_RING      (HDRG_SESSION_MAX_WINDOW + 4u) /* the sent buffers' stamps, in order */
#define HDRG_GST_SETTING   100u                           /* the property id of the first setting */
#define HDRG_GST_SINK_CAPS                                                                         \
    GST_VIDEO_CAPS_MAKE("{ I420, Y42B, Y444, I420_10LE, I422_10LE, Y444_10LE, I420_12LE, "         \
                        "I422_12LE, Y444_12LE, Y444_16LE, NV12, NV16, NV24, P010_10LE, P012_LE, "  \
                        "P016_LE, RGB, RGBx, RGBA, RGBA64_LE }")
#define HDRG_GST_SRC_CAPS GST_VIDEO_CAPS_MAKE("I420_10LE") ", colorimetry = (string) bt2100-pq"
#define HDRG_GST_DYNAMIC  "HdrgDynamicHdrMeta" /* the custom meta of the dynamic metadata */

enum {
    HDRG_GST_PROP_0,
    HDRG_GST_PROP_PRESET,
    HDRG_GST_PROP_TEMPORAL,
    HDRG_GST_PROP_PAST,
    HDRG_GST_PROP_FUTURE,
    HDRG_GST_PROP_PROXY,
    HDRG_GST_PROP_FLASH_LIMIT,
    HDRG_GST_PROP_DITHER,
    HDRG_GST_PROP_THREADS,
    HDRG_GST_PROP_LATENCY,
    HDRG_GST_PROP_OUT_RATE
};

typedef struct {
    GstBaseTransform parent;
    GstVideoInfo in_info, out_info;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    GstBuffer *stamps[HDRG_GST_RING]; /* the sent buffers' timestamps and flags, oldest first */
    GstBuffer *pending; /* the next output's buffer, kept while the session has no frame for it */
    uint32_t stamps_head, stamps_count, threads;
    uint32_t lag; /* the session's latency for a query (UINT32_MAX: none), under the object lock */
    gint out_num, out_den; /* out-rate (0: the input's) */
    uint8_t preset;
    bool ended; /* the end was sent: the next buffer starts the session again */
    bool late;  /* the latest frame came late (warned once a run of them) */
} hdrg_gst_t;

typedef struct {
    GstBaseTransformClass parent;
} hdrg_gst_class_t;

/* an input frame mapped while the session reads its pixels */
typedef struct {
    GstVideoFrame frame;
} hdrg_gst_frame_t;

static GstBaseTransformClass *hdrg_gst_parent;
static GstStaticPadTemplate hdrg_gst_sink = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(HDRG_GST_SINK_CAPS));
static GstStaticPadTemplate hdrg_gst_src =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(HDRG_GST_SRC_CAPS));

/* the session does not keep up: its report's time a frame and advice as a warning on the bus */
static void hdrg_gst_warn_late(hdrg_gst_t *self)
{
    hdrg_session_latency_t report;

    report.struct_size = (uint32_t)sizeof(report);

    if (hdrg_session_latency_report(self->session, &report) != HDRG_OK) {
        return;
    }

    GST_ELEMENT_WARNING(self, STREAM, FAILED,
                        ("hdrgrade does not keep up: %.1f ms a frame for %.1f",
                         (double)report.work_ms, (double)report.interval_ms),
                        ("prefilter %.1f, analyze %.1f, apply %.1f ms; advice 0x%x (HDRG_ADVICE_*)",
                         (double)report.prefilter_ms, (double)report.analyze_ms,
                         (double)report.apply_ms, report.advice));
}

static void hdrg_gst_release(void *opaque)
{
    hdrg_gst_frame_t *frame = (hdrg_gst_frame_t *)opaque;

    gst_video_frame_unmap(&frame->frame);
    g_free(frame);
}

static void hdrg_gst_close(hdrg_gst_t *self)
{
    uint32_t i;

    /* a query reads the latency, never the session destroyed here */
    GST_OBJECT_LOCK(self);
    self->lag = UINT32_MAX;
    GST_OBJECT_UNLOCK(self);
    hdrg_session_destroy(self->session);
    self->session = NULL;
    self->ended = false;
    self->late = false;

    for (i = 0; i < HDRG_GST_RING; i++) {
        gst_buffer_replace(&self->stamps[i], NULL);
    }

    gst_clear_buffer(&self->pending);
    self->stamps_head = 0;
    self->stamps_count = 0;
}

/* the element's context (made once, at the first need: the streaming thread's or a query's) */
static hdrg_status_t hdrg_gst_context(hdrg_gst_t *self)
{
    hdrg_context_desc_t desc;
    hdrg_status_t status;

    GST_OBJECT_LOCK(self);
    status = HDRG_OK;

    if (self->ctx == NULL) {
        hdrg_context_desc_init(&desc);
        desc.thread_count = self->threads;
        status = hdrg_context_create(&desc, &self->ctx);
    }

    GST_OBJECT_UNLOCK(self);

    return status;
}

static gboolean hdrg_gst_open(hdrg_gst_t *self)
{
    hdrg_status_t status;

    hdrg_gst_close(self);
    status = hdrg_gst_context(self);

    if (status == HDRG_OK) {
        status = hdrg_session_create(self->ctx, &self->format, &self->params, &self->caps,
                                     &self->config, &self->session);
    }

    if (status != HDRG_OK) {
        GST_ELEMENT_ERROR(self, LIBRARY, SETTINGS, ("hdrgrade: %s", hdrg_status_string(status)),
                          (NULL));

        return FALSE;
    }

    GST_OBJECT_LOCK(self);
    self->lag = hdrg_session_latency(self->session);
    GST_OBJECT_UNLOCK(self);

    return TRUE;
}

/* the input format's layout and bits (Y'CbCr) or bits and alpha (R'G'B'); false for others */
/* H.273's chroma location of a chroma site (unknown: LEFT, MPEG-2's) */
static uint8_t hdrg_gst_chroma(GstVideoChromaSite site)
{
    const bool h = (site & GST_VIDEO_CHROMA_SITE_H_COSITED) != 0,
               v = (site & GST_VIDEO_CHROMA_SITE_V_COSITED) != 0;

    if (site == GST_VIDEO_CHROMA_SITE_UNKNOWN) {
        return HDRG_CHROMA_LEFT;
    }

    if (h) {
        return v ? HDRG_CHROMA_TOPLEFT : HDRG_CHROMA_LEFT;
    }

    return v ? HDRG_CHROMA_TOP : HDRG_CHROMA_CENTER;
}

/*
 * the element's input formats: the layout (0: R'G'B'), bits, alpha and, for Y'CbCr, Cb and Cr
 * interleaved (NV12, P010) with the bits in the samples' high bits (P010, P012)
 */
static bool hdrg_gst_format(GstVideoFormat format, uint8_t *layout, uint8_t *bits, bool *alpha,
                            bool *interleaved, bool *msb)
{
    *alpha = false;
    *interleaved = format == GST_VIDEO_FORMAT_NV12 || format == GST_VIDEO_FORMAT_NV16 ||
                   format == GST_VIDEO_FORMAT_NV24 || format == GST_VIDEO_FORMAT_P010_10LE ||
                   format == GST_VIDEO_FORMAT_P012_LE || format == GST_VIDEO_FORMAT_P016_LE;
    *msb = format == GST_VIDEO_FORMAT_P010_10LE || format == GST_VIDEO_FORMAT_P012_LE;

    switch (format) {
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_I420_10LE:
    case GST_VIDEO_FORMAT_I420_12LE:
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P012_LE:
    case GST_VIDEO_FORMAT_P016_LE:
        *layout = HDRG_YUV_420;

        break;
    case GST_VIDEO_FORMAT_Y42B:
    case GST_VIDEO_FORMAT_I422_10LE:
    case GST_VIDEO_FORMAT_I422_12LE:
    case GST_VIDEO_FORMAT_NV16:
        *layout = HDRG_YUV_422;

        break;
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_Y444_10LE:
    case GST_VIDEO_FORMAT_Y444_12LE:
    case GST_VIDEO_FORMAT_Y444_16LE:
    case GST_VIDEO_FORMAT_NV24:
        *layout = HDRG_YUV_444;

        break;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_RGBA64_LE:
        *layout = 0;
        *alpha = format != GST_VIDEO_FORMAT_RGB;

        break;
    default:
        return false;
    }

    switch (format) {
    case GST_VIDEO_FORMAT_I420_10LE:
    case GST_VIDEO_FORMAT_I422_10LE:
    case GST_VIDEO_FORMAT_Y444_10LE:
    case GST_VIDEO_FORMAT_P010_10LE:
        *bits = 10;

        break;
    case GST_VIDEO_FORMAT_I420_12LE:
    case GST_VIDEO_FORMAT_I422_12LE:
    case GST_VIDEO_FORMAT_Y444_12LE:
    case GST_VIDEO_FORMAT_P012_LE:
        *bits = 12;

        break;
    case GST_VIDEO_FORMAT_Y444_16LE:
    case GST_VIDEO_FORMAT_P016_LE:
    case GST_VIDEO_FORMAT_RGBA64_LE:
        *bits = 16;

        break;
    default:
        *bits = 8;

        break;
    }

    return true;
}

/* the session's format and input pixels from the negotiated input; false with an error */
static gboolean hdrg_gst_input(hdrg_gst_t *self)
{
    const GstVideoColorimetry *color = &self->in_info.colorimetry;
    uint32_t primaries, transfer, matrix;
    uint8_t layout, bits;
    bool alpha, interleaved, msb;

    if (!hdrg_gst_format(GST_VIDEO_INFO_FORMAT(&self->in_info), &layout, &bits, &alpha,
                         &interleaved, &msb)) {
        return FALSE;
    }

    primaries = gst_video_color_primaries_to_iso(color->primaries);
    transfer = gst_video_transfer_function_to_iso(color->transfer);
    matrix = gst_video_color_matrix_to_iso(color->matrix);
    primaries = (primaries == 0 || primaries == 2) ? HDRG_PRIMARIES_BT709 : primaries;

    if (transfer == 16 || transfer == 18) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, ("hdrgrade: the input is HDR already"), (NULL));

        return FALSE;
    }

    if (transfer == 0 || transfer == 2) {
        transfer = (layout != 0) ? HDRG_TRANSFER_BT709 : HDRG_TRANSFER_SRGB;
    }

    if (matrix == 0 || matrix == 2) {
        matrix = (primaries == HDRG_PRIMARIES_BT2020) ? HDRG_MATRIX_BT2020
                 : (primaries == HDRG_PRIMARIES_BT601_625 || primaries == HDRG_PRIMARIES_BT601_525)
                     ? HDRG_MATRIX_BT601
                     : HDRG_MATRIX_BT709;
    }

    /* BT.470 BG and SMPTE 170M share the matrix */
    matrix = (matrix == 5u) ? HDRG_MATRIX_BT601 : matrix;

    memset(&self->format, 0, sizeof(self->format));
    self->format.width = (uint32_t)GST_VIDEO_INFO_WIDTH(&self->in_info);
    self->format.height = (uint32_t)GST_VIDEO_INFO_HEIGHT(&self->in_info);
    self->format.primaries = (uint8_t)HDRG_GST_MIN(primaries, 255u);
    self->format.transfer = (uint8_t)HDRG_GST_MIN(transfer, 255u);
    memset(&self->config.input, 0, sizeof(self->config.input));
    self->config.input.layout = layout;

    if (layout == 0) {
        self->format.bit_depth = bits;
        self->format.has_alpha = alpha;
        self->format.stride = (size_t)GST_VIDEO_INFO_PLANE_STRIDE(&self->in_info, 0);
    } else {
        self->config.input.matrix = (uint8_t)HDRG_GST_MIN(matrix, 255u);
        self->config.input.bits = bits;
        self->config.input.interleaved = interleaved;
        self->config.input.msb = msb;
        self->config.input.full_range = color->range == GST_VIDEO_COLOR_RANGE_0_255;
        self->config.input.chroma_loc = hdrg_gst_chroma(GST_VIDEO_INFO_CHROMA_SITE(&self->in_info));
    }

    return TRUE;
}

static GstCaps *hdrg_gst_transform_caps(GstBaseTransform *trans, GstPadDirection direction,
                                        GstCaps *caps, GstCaps *filter)
{
    hdrg_gst_t *self = HDRG_GST(trans);
    GstStructure *s;
    GstCaps *result, *narrowed;
    guint i;

    result = gst_caps_new_empty();

    for (i = 0; i < gst_caps_get_size(caps); i++) {
        s = gst_structure_copy(gst_caps_get_structure(caps, i));
        gst_structure_remove_fields(s, "format", "colorimetry", "chroma-site",
                                    "mastering-display-info", "content-light-level", NULL);

        if (direction == GST_PAD_SINK) {
            gst_structure_set(s, "format", G_TYPE_STRING, "I420_10LE", "colorimetry", G_TYPE_STRING,
                              "bt2100-pq", "chroma-site", G_TYPE_STRING, "mpeg2", NULL);
        }

        /* out-rate: the source pad's rate its own, the sink pad's any */
        if (self->out_num > 0 && direction == GST_PAD_SINK) {
            gst_structure_set(s, "framerate", GST_TYPE_FRACTION, self->out_num, self->out_den,
                              NULL);
        } else if (self->out_num > 0) {
            gst_structure_set(s, "framerate", GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1,
                              NULL);
        }

        gst_caps_append_structure(result, s);
    }

    if (filter == NULL) {
        return result;
    }

    narrowed = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);

    return narrowed;
}

/* the output caps carry the mastering display: Display P3 D65 up to the display's peak */
static GstCaps *hdrg_gst_fixate_caps(GstBaseTransform *trans, GstPadDirection direction,
                                     GstCaps *caps, GstCaps *othercaps)
{
    const guint16 primaries[3][2] = {{34000, 16000}, {13250, 34500}, {7500, 3000}};
    hdrg_gst_t *self = HDRG_GST(trans);
    GstVideoMasteringDisplayInfo master;
    GstCaps *fixed;
    uint8_t i;

    fixed = hdrg_gst_parent->fixate_caps(trans, direction, caps, othercaps);
    if (direction != GST_PAD_SINK || fixed == NULL) {
        return fixed;
    }

    gst_video_mastering_display_info_init(&master);

    for (i = 0; i < 3u; i++) {
        master.display_primaries[i].x = primaries[i][0];
        master.display_primaries[i].y = primaries[i][1];
    }

    master.white_point.x = 15635;
    master.white_point.y = 16450;
    master.max_display_mastering_luminance =
        (guint32)(hdrg_display_caps_peak(&self->caps) * 10000.0f + 0.5f);
    master.min_display_mastering_luminance = 50;

    fixed = gst_caps_make_writable(fixed);
    gst_video_mastering_display_info_add_to_caps(&master, fixed);

    return fixed;
}

static gboolean hdrg_gst_set_caps(GstBaseTransform *trans, GstCaps *incaps, GstCaps *outcaps)
{
    const GstVideoInfo *in = &HDRG_GST(trans)->in_info;
    hdrg_gst_t *self = HDRG_GST(trans);

    if (!gst_video_info_from_caps(&self->in_info, incaps) ||
        !gst_video_info_from_caps(&self->out_info, outcaps) || !hdrg_gst_input(self)) {
        return FALSE;
    }

    self->config.output.layout = HDRG_YUV_420;
    self->config.output.matrix = HDRG_MATRIX_BT2020;
    self->config.output.bits = 10;
    self->config.output.full_range = false;
    self->caps.gamut = HDRG_GAMUT_BT2020;
    self->config.rate_num = (GST_VIDEO_INFO_FPS_N(in) > 0) ? (uint32_t)GST_VIDEO_INFO_FPS_N(in) : 0;
    self->config.rate_den = (GST_VIDEO_INFO_FPS_N(in) > 0) ? (uint32_t)GST_VIDEO_INFO_FPS_D(in) : 0;
    self->config.out_rate_num = (self->out_num > 0) ? (uint32_t)self->out_num : 0u;
    self->config.out_rate_den = (self->out_num > 0) ? (uint32_t)self->out_den : 0u;

    return hdrg_gst_open(self);
}

static GstFlowReturn hdrg_gst_submit(GstBaseTransform *trans, gboolean is_discont, GstBuffer *inbuf)
{
    hdrg_gst_t *self = HDRG_GST(trans);
    hdrg_session_frame_t in;
    hdrg_gst_frame_t *frame;
    hdrg_status_t status;
    GstBuffer *stamp;
    guint p;

    (void)is_discont;

    if ((self->session == NULL || self->ended) && !hdrg_gst_open(self)) {
        gst_buffer_unref(inbuf);

        return GST_FLOW_NOT_NEGOTIATED;
    }

    frame = g_new0(hdrg_gst_frame_t, 1);
    if (!gst_video_frame_map(&frame->frame, &self->in_info, inbuf, GST_MAP_READ)) {
        g_free(frame);
        gst_buffer_unref(inbuf);

        return GST_FLOW_ERROR;
    }

    memset(&in, 0, sizeof(in));

    for (p = 0; p < GST_VIDEO_FRAME_N_PLANES(&frame->frame) && p < 3u; p++) {
        in.planes[p] = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&frame->frame, p);
        in.strides[p] = (size_t)GST_VIDEO_FRAME_PLANE_STRIDE(&frame->frame, p);
    }

    in.pts = (int64_t)GST_BUFFER_PTS(inbuf);
    in.release = hdrg_gst_release;

    /* out-rate: the frames between are placed by the buffers' timestamps */
    if ((self->out_num > 0 && !GST_BUFFER_PTS_IS_VALID(inbuf)) ||
        self->stamps_count == HDRG_GST_RING) {
        hdrg_gst_release(frame);
        gst_buffer_unref(inbuf);
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("hdrgrade: a buffer without a timestamp"), (NULL));

        return GST_FLOW_ERROR;
    }

    in.opaque = frame;

    stamp = gst_buffer_new();
    gst_buffer_copy_into(stamp, inbuf, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0,
                         G_MAXSIZE);
    gst_buffer_unref(inbuf);

    status = (self->config.input.layout == 0 && in.strides[0] != self->format.stride)
                 ? HDRG_ERR_INVALID_ARG
                 : hdrg_session_send_frame(self->session, &in);
    if (status != HDRG_OK) {
        hdrg_gst_release(frame);
        gst_buffer_unref(stamp);
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("hdrgrade: %s", hdrg_status_string(status)),
                          (NULL));

        return GST_FLOW_ERROR;
    }

    self->stamps[(self->stamps_head + self->stamps_count) % HDRG_GST_RING] = stamp;
    self->stamps_count++;

    return GST_FLOW_OK;
}

/* the session's next output, or none yet (outbuf NULL) */
/* the output's T.35 payloads in buf's HdrgDynamicHdrMeta (neither measured: no meta) */
static void hdrg_gst_dynamic(GstBuffer *buf, const hdrg_session_output_t *out)
{
    GstCustomMeta *meta;
    GstStructure *fields;
    GstBuffer *bytes;
    uint8_t plus[HDRG_HDR10PLUS_T35_BYTES], curve[HDRG_ST2094_50_T35_MAX];
    size_t plus_size, curve_size;

    plus_size = hdrg_hdr10plus_t35(&out->hdr10plus, plus, sizeof(plus));
    curve_size = hdrg_st2094_50_t35(&out->st2094_50, curve, sizeof(curve));

    if (plus_size == 0 && curve_size == 0) {
        return;
    }

    meta = gst_buffer_add_custom_meta(buf, HDRG_GST_DYNAMIC);
    if (meta == NULL) {
        return;
    }

    fields = gst_custom_meta_get_structure(meta);

    if (plus_size > 0) {
        bytes = gst_buffer_new_memdup(plus, plus_size);
        gst_structure_set(fields, "hdr10plus", GST_TYPE_BUFFER, bytes, NULL);
        gst_buffer_unref(bytes);
    }

    if (curve_size > 0) {
        bytes = gst_buffer_new_memdup(curve, curve_size);
        gst_structure_set(fields, "st2094-50", GST_TYPE_BUFFER, bytes, NULL);
        gst_buffer_unref(bytes);
    }
}

/* a buffer for an output: the negotiated pool's (hdrg_gst_decide_allocation), else allocated */
static GstFlowReturn hdrg_gst_buffer(GstBaseTransform *trans, const GstVideoInfo *info,
                                     GstBuffer **out)
{
    GstAllocationParams params;
    GstAllocator *allocator;
    GstBufferPool *pool;
    GstFlowReturn ret;

    *out = NULL;
    pool = gst_base_transform_get_buffer_pool(trans);

    if (pool != NULL) {
        ret = (gst_buffer_pool_is_active(pool) || gst_buffer_pool_set_active(pool, TRUE))
                  ? gst_buffer_pool_acquire_buffer(pool, out, NULL)
                  : GST_FLOW_ERROR;
        gst_object_unref(pool);

        return ret;
    }

    gst_base_transform_get_allocator(trans, &allocator, &params);
    *out = gst_buffer_new_allocate(allocator, GST_VIDEO_INFO_SIZE(info), &params);

    if (allocator != NULL) {
        gst_object_unref(allocator);
    }

    return (*out != NULL) ? GST_FLOW_OK : GST_FLOW_ERROR;
}

static GstFlowReturn hdrg_gst_generate(GstBaseTransform *trans, GstBuffer **outbuf)
{
    hdrg_gst_t *self = HDRG_GST(trans);
    hdrg_session_output_t out;
    hdrg_status_t status;
    GstVideoFrame frame;
    GstFlowReturn ret;
    GstBuffer *buf, *stamp;
    guint p;
    int64_t want;

    *outbuf = NULL;

    if (self->session == NULL) {
        return GST_FLOW_OK;
    }

    /* the next output's buffer, kept while the session has no frame for it yet */
    if (self->pending == NULL) {
        ret = hdrg_gst_buffer(trans, &self->out_info, &self->pending);
        if (ret != GST_FLOW_OK) {
            return ret;
        }
    }

    buf = self->pending;

    if (!gst_video_frame_map(&frame, &self->out_info, buf, GST_MAP_WRITE)) {
        return GST_FLOW_ERROR;
    }

    memset(&out, 0, sizeof(out));

    for (p = 0; p < 3u; p++) {
        out.planes[p] = (uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&frame, p);
        out.strides[p] = (size_t)GST_VIDEO_FRAME_PLANE_STRIDE(&frame, p);
    }

    status = hdrg_session_receive_frame(self->session, &out);
    gst_video_frame_unmap(&frame);

    if (status == HDRG_ERR_AGAIN) {
        return GST_FLOW_OK;
    }

    /* the end: no output needs the buffer */
    if (status == HDRG_EOF) {
        gst_clear_buffer(&self->pending);

        return GST_FLOW_OK;
    }

    self->pending = NULL;

    if (status != HDRG_OK) {
        gst_buffer_unref(buf);
        GST_ELEMENT_ERROR(self, STREAM, FAILED, ("hdrgrade: %s", hdrg_status_string(status)),
                          (NULL));

        return GST_FLOW_ERROR;
    }

    /* frames that come late (the session does not keep up): a warning with its advice */
    if (out.late && !self->late) {
        hdrg_gst_warn_late(self);
    }

    self->late = out.late;

    /* the input buffer's stamp (a frame between: the nearer one's), those before it dropped */
    want = !out.synthetic ? out.pts : out.src_pts[(out.phase < 32768u) ? 0 : 1];

    while (self->stamps_count > 1u &&
           (int64_t)GST_BUFFER_PTS(self->stamps[self->stamps_head]) < want) {
        gst_buffer_replace(&self->stamps[self->stamps_head], NULL);
        self->stamps_head = (self->stamps_head + 1u) % HDRG_GST_RING;
        self->stamps_count--;
    }

    stamp = (self->stamps_count > 0u) ? self->stamps[self->stamps_head] : NULL;

    if (stamp != NULL) {
        gst_buffer_copy_into(buf, stamp, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0,
                             G_MAXSIZE);
    }

    /* without out-rate one output a buffer, in order (timestamps or none): its stamp done */
    if (self->out_num == 0 && self->stamps_count > 0u) {
        gst_buffer_replace(&self->stamps[self->stamps_head], NULL);
        self->stamps_head = (self->stamps_head + 1u) % HDRG_GST_RING;
        self->stamps_count--;
    }

    /* out-rate: the output's own time and length */
    if (self->out_num > 0) {
        GST_BUFFER_PTS(buf) = (GstClockTime)out.pts;
        GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buf) =
            gst_util_uint64_scale(GST_SECOND, (guint64)self->out_den, (guint64)self->out_num);
    }

    hdrg_gst_dynamic(buf, &out);
    *outbuf = buf;

    return GST_FLOW_OK;
}

/* EOS: the end into the session and every frame it holds out; a flush starts it again */
static gboolean hdrg_gst_sink_event(GstBaseTransform *trans, GstEvent *event)
{
    hdrg_gst_t *self = HDRG_GST(trans);
    GstFlowReturn ret;
    GstBuffer *buf;

    if (GST_EVENT_TYPE(event) == GST_EVENT_EOS && self->session != NULL && !self->ended) {
        self->ended = true;
        (void)hdrg_session_send_frame(self->session, NULL);

        do {
            ret = hdrg_gst_generate(trans, &buf);
            if (ret == GST_FLOW_OK && buf != NULL) {
                ret = gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(trans), buf);
            }
        } while (ret == GST_FLOW_OK && buf != NULL);
    }

    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP && self->session != NULL) {
        hdrg_gst_close(self);
    }

    return hdrg_gst_parent->sink_event(trans, event);
}

/* the latency upstream reports plus the frames an output waits for (the window's future) */
static gboolean hdrg_gst_query(GstBaseTransform *trans, GstPadDirection direction, GstQuery *query)
{
    hdrg_gst_t *self = HDRG_GST(trans);
    hdrg_session_config_t config;
    GstClockTime min, max, frame;
    gboolean live, ret;
    uint32_t lag;

    ret = hdrg_gst_parent->query(trans, direction, query);
    if (!ret || direction != GST_PAD_SRC || GST_QUERY_TYPE(query) != GST_QUERY_LATENCY ||
        GST_VIDEO_INFO_FPS_N(&self->in_info) <= 0) {
        return ret;
    }

    gst_query_parse_latency(query, &live, &min, &max);
    frame = gst_util_uint64_scale(GST_SECOND, (guint64)GST_VIDEO_INFO_FPS_D(&self->in_info),
                                  (guint64)GST_VIDEO_INFO_FPS_N(&self->in_info));
    config = self->config;
    GST_OBJECT_LOCK(self);
    lag = self->lag;
    GST_OBJECT_UNLOCK(self);

    /* no session yet: the configuration's future (the context made, resolved under the lock) */
    if (lag == UINT32_MAX) {
        lag = 0;

        if (hdrg_gst_context(self) == HDRG_OK) {
            GST_OBJECT_LOCK(self);

            if (self->ctx != NULL) {
                hdrg_session_config_resolve(self->ctx, &config);
                lag = config.future;
            }

            GST_OBJECT_UNLOCK(self);
        }
    }

    min += frame * lag;

    if (max != GST_CLOCK_TIME_NONE) {
        max += frame * lag;
    }

    gst_query_set_latency(query, live, min, max);

    return TRUE;
}

/* the source pad's buffers from a pool (downstream's or one of the base class), each a frame */
static gboolean hdrg_gst_decide_allocation(GstBaseTransform *trans, GstQuery *query)
{
    const guint want = (guint)GST_VIDEO_INFO_SIZE(&HDRG_GST(trans)->out_info);
    GstBufferPool *pool;
    guint size, min, max;

    if (gst_query_get_n_allocation_pools(query) == 0) {
        gst_query_add_allocation_pool(query, NULL, want, 0, 0);
    } else {
        gst_query_parse_nth_allocation_pool(query, 0, &pool, &size, &min, &max);
        gst_query_set_nth_allocation_pool(query, 0, pool, (size > want) ? size : want, min, max);

        if (pool != NULL) {
            gst_object_unref(pool);
        }
    }

    return hdrg_gst_parent->decide_allocation(trans, query);
}

/*
 * The session reads R'G'B' frames until it outputs them (Y'CbCr ones it
 * converts at once): upstream's pool keeps its latency and the frame sent
 * besides, a small pool of its own (a camera's, a decoder's) not stalling
 */
static gboolean hdrg_gst_propose_allocation(GstBaseTransform *trans, GstQuery *decide_query,
                                            GstQuery *query)
{
    hdrg_gst_t *self = HDRG_GST(trans);
    GstVideoInfo info;
    GstCaps *caps;
    uint32_t lag;

    /* (none: a query after the first, which the base class would pass downstream) */
    if (decide_query != NULL && !hdrg_gst_parent->propose_allocation(trans, decide_query, query)) {
        return FALSE;
    }

    gst_query_parse_allocation(query, &caps, NULL);
    GST_OBJECT_LOCK(self);
    lag = self->lag;
    GST_OBJECT_UNLOCK(self);

    if (caps != NULL && lag != UINT32_MAX && gst_query_get_n_allocation_pools(query) == 0 &&
        gst_video_info_from_caps(&info, caps) && GST_VIDEO_INFO_IS_RGB(&info)) {
        gst_query_add_allocation_pool(query, NULL, (guint)GST_VIDEO_INFO_SIZE(&info), lag + 1u, 0);
    }

    return TRUE;
}

static gboolean hdrg_gst_stop(GstBaseTransform *trans)
{
    hdrg_gst_t *self = HDRG_GST(trans);
    hdrg_context_t *ctx;

    hdrg_gst_close(self);
    GST_OBJECT_LOCK(self);
    ctx = self->ctx;
    self->ctx = NULL;
    GST_OBJECT_UNLOCK(self);
    hdrg_context_destroy(ctx);

    return TRUE;
}

static void hdrg_gst_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
    const gchar *name;
    hdrg_gst_t *self = HDRG_GST(object);
    hdrg_setting_t setting;
    double number;

    switch (id) {
    case HDRG_GST_PROP_PRESET:
        /* the library's names (hdrg_setting_parse): the settings initialized for it */
        name = g_value_get_string(value);
        if (name != NULL && hdrg_setting_parse("preset", name, &number) == HDRG_OK &&
            hdrg_setting_set("preset", number, &self->params, NULL, NULL) == HDRG_OK) {
            self->preset = self->params.preset;
        }

        break;
    case HDRG_GST_PROP_TEMPORAL:
        /* (TWO_PASS reads the stream twice: not an element's) */
        name = g_value_get_string(value);
        if (name != NULL && hdrg_setting_parse("temporal", name, &number) == HDRG_OK &&
            number != (double)HDRG_TEMPORAL_TWO_PASS) {
            (void)hdrg_session_setting_set("temporal", number, &self->config);
        }

        break;
    case HDRG_GST_PROP_PAST:
        self->config.past = (uint16_t)g_value_get_uint(value);

        break;
    case HDRG_GST_PROP_FUTURE:
        self->config.future = (uint16_t)g_value_get_uint(value);

        break;
    case HDRG_GST_PROP_PROXY:
        self->config.proxy = (uint16_t)g_value_get_uint(value);

        break;
    case HDRG_GST_PROP_FLASH_LIMIT:
        self->config.flash_off = !g_value_get_boolean(value);

        break;
    case HDRG_GST_PROP_DITHER:
        self->config.dither_off = !g_value_get_boolean(value);

        break;
    case HDRG_GST_PROP_THREADS:
        self->threads = g_value_get_uint(value);

        break;
    case HDRG_GST_PROP_LATENCY:
        name = g_value_get_string(value);
        if (name != NULL && hdrg_setting_parse("latency", name, &number) == HDRG_OK) {
            (void)hdrg_session_setting_set("latency", number, &self->config);
        }

        break;
    case HDRG_GST_PROP_OUT_RATE:
        self->out_num = gst_value_get_fraction_numerator(value);
        self->out_den = gst_value_get_fraction_denominator(value);
        gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));

        break;
    default:
        if (id < HDRG_GST_SETTING || !hdrg_setting_at(id - HDRG_GST_SETTING, &setting)) {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);

            break;
        }

        number = (setting.kind == HDRG_SETTING_FLOAT)  ? (double)g_value_get_float(value)
                 : (setting.kind == HDRG_SETTING_BOOL) ? (g_value_get_boolean(value) ? 1.0 : 0.0)
                                                       : (double)g_value_get_uint(value);
        (void)hdrg_setting_set(setting.name, number, &self->params, &self->caps, NULL);

        break;
    }
}

static void hdrg_gst_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
    const hdrg_gst_t *self = HDRG_GST(object);
    hdrg_setting_t setting;
    double number;

    switch (id) {
    case HDRG_GST_PROP_PRESET:
        g_value_set_string(value, hdrg_setting_value_name("preset", (double)self->preset));

        break;
    case HDRG_GST_PROP_TEMPORAL:
        g_value_set_string(value,
                           hdrg_setting_value_name("temporal", (double)self->config.temporal));

        break;
    case HDRG_GST_PROP_PAST:
        g_value_set_uint(value, self->config.past);

        break;
    case HDRG_GST_PROP_FUTURE:
        g_value_set_uint(value, self->config.future);

        break;
    case HDRG_GST_PROP_PROXY:
        g_value_set_uint(value, self->config.proxy);

        break;
    case HDRG_GST_PROP_FLASH_LIMIT:
        g_value_set_boolean(value, !self->config.flash_off);

        break;
    case HDRG_GST_PROP_DITHER:
        g_value_set_boolean(value, !self->config.dither_off);

        break;
    case HDRG_GST_PROP_THREADS:
        g_value_set_uint(value, self->threads);

        break;
    case HDRG_GST_PROP_LATENCY:
        g_value_set_string(value, hdrg_setting_value_name("latency", (double)self->config.latency));

        break;
    case HDRG_GST_PROP_OUT_RATE:
        gst_value_set_fraction(value, self->out_num, (self->out_num > 0) ? self->out_den : 1);

        break;
    default:
        if (id < HDRG_GST_SETTING || !hdrg_setting_at(id - HDRG_GST_SETTING, &setting) ||
            hdrg_setting_get(setting.name, &self->params, &self->caps, NULL, &number) != HDRG_OK) {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);

            break;
        }

        if (setting.kind == HDRG_SETTING_FLOAT) {
            g_value_set_float(value, (gfloat)number);
        } else if (setting.kind == HDRG_SETTING_BOOL) {
            g_value_set_boolean(value, number != 0.0);
        } else {
            g_value_set_uint(value, (guint)number);
        }

        break;
    }
}

static void hdrg_gst_finalize(GObject *object)
{
    hdrg_gst_t *self = HDRG_GST(object);

    hdrg_gst_close(self);
    hdrg_context_destroy(self->ctx);
    G_OBJECT_CLASS(hdrg_gst_parent)->finalize(object);
}

/* the content intent and display settings as properties (hdrg-cli's names), their defaults */
static void hdrg_gst_install_settings(GObjectClass *object_class)
{
    hdrg_display_caps_t caps;
    hdrg_params_t params;
    hdrg_setting_t setting;
    GParamSpec *spec;
    uint32_t i;
    double value;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_display_caps_init(&caps);

    for (i = 0; hdrg_setting_at(i, &setting); i++) {
        /* (the preset and a session's are the element's own properties) */
        if (setting.part == HDRG_SETTING_ENCODE || strcmp(setting.name, "preset") == 0 ||
            hdrg_setting_get(setting.name, &params, &caps, NULL, &value) != HDRG_OK) {
            continue;
        }

        if (setting.kind == HDRG_SETTING_FLOAT) {
            spec = g_param_spec_float(setting.name, setting.name, "hdrg-cli setting", -G_MAXFLOAT,
                                      G_MAXFLOAT, (gfloat)value,
                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
        } else if (setting.kind == HDRG_SETTING_BOOL) {
            spec = g_param_spec_boolean(setting.name, setting.name, "hdrg-cli setting",
                                        value != 0.0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
        } else {
            spec = g_param_spec_uint(setting.name, setting.name, "hdrg-cli setting", 0,
                                     (setting.kind == HDRG_SETTING_U8)    ? 255u
                                     : (setting.kind == HDRG_SETTING_U16) ? 65535u
                                                                          : G_MAXUINT32,
                                     (guint)value, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
        }

        g_object_class_install_property(object_class, HDRG_GST_SETTING + i, spec);
    }
}

static void hdrg_gst_class_init(gpointer klass, gpointer data)
{
    const GParamFlags flags = (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    const gchar *tags[] = {GST_META_TAG_VIDEO_STR, GST_META_TAG_VIDEO_COLORSPACE_STR, NULL};
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS(klass);

    (void)data;

    /* the dynamic metadata's meta: dropped by the elements that change the colors or know it not */
    if (gst_meta_get_info(HDRG_GST_DYNAMIC) == NULL) {
        (void)gst_meta_register_custom(HDRG_GST_DYNAMIC, tags, NULL, NULL, NULL);
    }

    hdrg_gst_parent = (GstBaseTransformClass *)g_type_class_peek_parent(klass);
    object_class->set_property = hdrg_gst_set_property;
    object_class->get_property = hdrg_gst_get_property;
    object_class->finalize = hdrg_gst_finalize;

    g_object_class_install_property(
        object_class, HDRG_GST_PROP_PRESET,
        g_param_spec_string("preset", "preset", "photo, anime or game (resets the settings)",
                            "photo", flags));
    g_object_class_install_property(object_class, HDRG_GST_PROP_TEMPORAL,
                                    g_param_spec_string("temporal", "temporal",
                                                        "windowed, recursive or static", "windowed",
                                                        flags));
    g_object_class_install_property(
        object_class, HDRG_GST_PROP_PAST,
        g_param_spec_uint("past", "past",
                          "frames before a frame its statistics average over (65535 = the "
                          "library's default: 8 with the temporal statistics, else 0)",
                          0, HDRG_SESSION_AUTO, HDRG_SESSION_AUTO, flags));
    g_object_class_install_property(
        object_class, HDRG_GST_PROP_FUTURE,
        g_param_spec_uint("future", "future",
                          "frames after it (the latency; 65535 = the past's, low and the modes "
                          "but windowed 0)",
                          0, HDRG_SESSION_AUTO, HDRG_SESSION_AUTO, flags));
    g_object_class_install_property(
        object_class, HDRG_GST_PROP_PROXY,
        g_param_spec_uint("proxy", "proxy", "longest side of the analysis image (0 = full)", 0,
                          65535, 960, flags));
    g_object_class_install_property(object_class, HDRG_GST_PROP_FLASH_LIMIT,
                                    g_param_spec_boolean("flash-limit", "flash-limit",
                                                         "the flash limiter (BT.1702-3)", TRUE,
                                                         flags));
    g_object_class_install_property(
        object_class, HDRG_GST_PROP_DITHER,
        g_param_spec_boolean("dither", "dither", "dither the output", TRUE, flags));
    g_object_class_install_property(object_class, HDRG_GST_PROP_THREADS,
                                    g_param_spec_uint("threads", "threads",
                                                      "the library's threads (0 = one per core)", 0,
                                                      1024, 0, flags));
    g_object_class_install_property(
        object_class, HDRG_GST_PROP_LATENCY,
        g_param_spec_string("latency", "latency",
                            "playback (watched: keeps up on average), low (the low-latency mode: "
                            "each frame within its interval, no future) or offline (a file nobody "
                            "watches: never late)",
                            "playback", flags));
    g_object_class_install_property(
        object_class, HDRG_GST_PROP_OUT_RATE,
        gst_param_spec_fraction("out-rate", "out-rate",
                                "the source pad's frame rate: the frames between the input's "
                                "interpolated (0/1: the input's)",
                                0, 1, G_MAXINT, 1, 0, 1, flags));
    hdrg_gst_install_settings(object_class);

    gst_element_class_set_static_metadata(
        element_class, "HDR grading", "Filter/Effect/Converter/Video",
        "Grades SDR video into HDR (single-layer PQ)", "libhdrgrading");
    gst_element_class_add_static_pad_template(element_class, &hdrg_gst_sink);
    gst_element_class_add_static_pad_template(element_class, &hdrg_gst_src);

    trans_class->passthrough_on_same_caps = FALSE;
    trans_class->transform_caps = hdrg_gst_transform_caps;
    trans_class->fixate_caps = hdrg_gst_fixate_caps;
    trans_class->set_caps = hdrg_gst_set_caps;
    trans_class->submit_input_buffer = hdrg_gst_submit;
    trans_class->generate_output = hdrg_gst_generate;
    trans_class->sink_event = hdrg_gst_sink_event;
    trans_class->query = hdrg_gst_query;
    trans_class->decide_allocation = hdrg_gst_decide_allocation;
    trans_class->propose_allocation = hdrg_gst_propose_allocation;
    trans_class->stop = hdrg_gst_stop;
}

static void hdrg_gst_init(GTypeInstance *instance, gpointer klass)
{
    hdrg_gst_t *self = HDRG_GST(instance);

    (void)klass;

    self->preset = HDRG_PRESET_PHOTO;
    self->lag = UINT32_MAX;
    hdrg_params_init(&self->params, self->preset);
    hdrg_display_caps_init(&self->caps);
    hdrg_session_config_init(&self->config);
}

static GType hdrg_gst_get_type(void)
{
    static gsize type = 0;
    GType registered;

    if (g_once_init_enter(&type)) {
        registered = g_type_register_static_simple(
            GST_TYPE_BASE_TRANSFORM, "HdrgGrade", sizeof(hdrg_gst_class_t), hdrg_gst_class_init,
            sizeof(hdrg_gst_t), hdrg_gst_init, (GTypeFlags)0);
        g_once_init_leave(&type, registered);
    }

    return (GType)type;
}

static gboolean hdrg_gst_plugin_init(GstPlugin *plugin)
{
    return gst_element_register(plugin, "hdrgrade", GST_RANK_NONE, hdrg_gst_get_type());
}

/* the plugin's descriptor, as GST_PLUGIN_DEFINE makes it (its reserved fields initialized) */
static const GstPluginDesc hdrg_gst_desc = {GST_VERSION_MAJOR,
                                            GST_VERSION_MINOR,
                                            "hdrgrading",
                                            "SDR to HDR grading (libhdrgrading)",
                                            hdrg_gst_plugin_init,
                                            HDRG_VERSION_STRING,
                                            "MIT/X11",
                                            "libhdrgrading",
                                            "libhdrgrading",
                                            "https://github.com/zeriyoshi/libhdrgrading",
                                            NULL,
                                            GST_PADDING_INIT};

extern GST_PLUGIN_EXPORT const GstPluginDesc *gst_plugin_hdrgrading_get_desc(void);

extern GST_PLUGIN_EXPORT const GstPluginDesc *gst_plugin_hdrgrading_get_desc(void)
{
    return &hdrg_gst_desc;
}
