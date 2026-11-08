/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <libavutil/buffer.h>
#include <libavutil/film_grain_params.h>
#include <libavutil/frame.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/version.h>

#include "hdrg_ffmpeg.h"
#include "../test_util.h"

/*
 * tools/ffmpeg: the session formats of AVFrame pixel
 * formats and colors, the formats refused, frames by reference (no copy, the
 * reference dropped on release), a session through the binding equals one
 * fed the same pixels directly (its output frames from a pool, one kept
 * while the session has none for it), frames from a pool (aligned, a
 * buffer given again only once unreferenced), the output's colors and mastering display,
 * HDR10+ and ST 2094-50 (FFmpeg 9) on the output (their values, T.35), the side data an output
 * keeps, and AV1's film grain parameters the decoder left out going with the frame as
 * HDRG_SD_FILM_GRAIN (H.274's and broken ones not).
 */
#define TF_W      64
#define TF_H      48
#define TF_FRAMES 12

/* the HDR10+ percentiles' percentages (hdrg_hdr10plus_t) */
static const uint8_t tf_percentages[HDRG_HDR10PLUS_PERCENTILES] = {1,  5,  10, 25, 50,
                                                                   75, 90, 95, 99};

typedef struct {
    int32_t format, range, space, primaries, trc;
    uint8_t layout, bits, matrix, prim_out, trc_out;
    bool full, alpha;
} tf_case_t;

/* a frame of format with the given colors, its samples a gradient that moves with t */
static AVFrame *tf_frame(int32_t format, int32_t t)
{
    AVFrame *frame;
    uint16_t *row16;
    uint8_t *row;
    int32_t p, x, y, w, h, value;

    frame = av_frame_alloc();
    TEST_ASSERT_NOT_NULL(frame);
    frame->format = format;
    frame->width = TF_W;
    frame->height = TF_H;
    TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));
    frame->pts = t;

    /* P010: the yuv420p10 frame's samples in the high bits, Cb and Cr alternating */
    for (p = 0; format == AV_PIX_FMT_P010 && p < 2; p++) {
        for (y = 0; y < ((p == 0) ? TF_H : TF_H / 2); y++) {
            row16 = (uint16_t *)(void *)(frame->data[p] + y * frame->linesize[p]);

            for (x = 0; x < ((p == 0) ? TF_W : TF_W / 2); x++) {
                value = (p == 0) ? 64 + (x * 11 + y * 7 + t * 5) % 876 : 448 + (x + y + t) % 128;

                if (p == 0) {
                    row16[x] = (uint16_t)(value << 6);
                } else {
                    row16[2 * x] = (uint16_t)(value << 6);
                    row16[2 * x + 1] = (uint16_t)(value << 6);
                }
            }
        }
    }

    for (p = 0; p < 4 && frame->data[p] != NULL && format != AV_PIX_FMT_P010; p++) {
        w = (format == AV_PIX_FMT_YUV420P10 && p > 0) ? TF_W / 2 : TF_W;
        h = (format == AV_PIX_FMT_YUV420P10 && p > 0) ? TF_H / 2 : TF_H;

        for (y = 0; y < h; y++) {
            row = frame->data[p] + y * frame->linesize[p];
            row16 = (uint16_t *)(void *)row;

            for (x = 0; x < frame->linesize[p] && format == AV_PIX_FMT_RGB24; x++) {
                row[x] = (uint8_t)((x * 5 + y * 3 + t * 7) % 256);
            }

            for (x = 0; x < w && format == AV_PIX_FMT_YUV420P10; x++) {
                value = (p == 0) ? 64 + (x * 11 + y * 7 + t * 5) % 876 : 448 + (x + y + t) % 128;
                row16[x] = (uint16_t)value;
            }
        }
    }

    return frame;
}

/*
 * Frames of format through a session, by the binding (frames and output) or
 * directly (the frames kept to the end: R'G'B' is read until it renders)
 */
static void tf_run(int32_t format, const hdrg_session_pixfmt_t *output, bool binding, uint8_t *rows,
                   size_t frame_bytes)
{
    hdrg_session_config_t config;
    hdrg_display_caps_t caps;
    hdrg_session_format_t raw;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    AVBufferPool *pool;
    AVFrame *src, *dst, *first, *kept[TF_FRAMES];
    uint8_t p;
    int32_t sent, received, y, h, row;

    first = tf_frame(format, 0);
    hdrg_session_config_init(&config);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(first, &raw, &config.input));
    config.output = *output;
    config.past = 0;
    hdrg_display_caps_init(&caps);
    caps.gamut = (output->layout != 0) ? HDRG_GAMUT_BT2020 : HDRG_GAMUT_P3;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, &raw, NULL, &caps, &config, &session));
    av_frame_free(&first);

    sent = 0;
    received = 0;
    dst = NULL;
    pool = NULL;
    memset(kept, 0, sizeof(kept));

    /* the binding's frames from a pool, one kept while the session has none for it */
    while (received < TF_FRAMES) {
        if (binding) {
            dst = (dst != NULL) ? dst : hdrg_ff_output_frame(TF_W, TF_H, output, &pool, &out);
            TEST_ASSERT_NOT_NULL(dst);
            memset(&out, 0, sizeof(out));

            for (p = 0; p < 3u; p++) {
                out.planes[p] = dst->data[p];
                out.strides[p] = (size_t)dst->linesize[p];
            }
        } else {
            memset(&out, 0, sizeof(out));
            out.planes[0] = rows + (size_t)received * frame_bytes;
            out.strides[0] = (size_t)TF_W * ((output->layout != 0) ? 2u : 6u);
            out.planes[1] = out.planes[0] + (size_t)TF_W * TF_H * 2u;
            out.planes[2] = out.planes[1] + (size_t)TF_W * TF_H / 2u;
            out.strides[1] = (size_t)TF_W;
            out.strides[2] = (size_t)TF_W;
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            src = (sent < TF_FRAMES) ? tf_frame(format, sent) : NULL;

            if (src != NULL && binding) {
                TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_frame(src, &raw, &config.input, &in));
            } else if (src != NULL) {
                memset(&in, 0, sizeof(in));

                for (p = 0; p < 3u; p++) {
                    in.planes[p] = src->data[p];
                    in.strides[p] = (size_t)src->linesize[p];
                }

                in.pts = src->pts;
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (src != NULL) ? &in : NULL));

            if (binding || src == NULL) {
                av_frame_free(&src);
            } else {
                kept[sent] = src;
            }

            sent++;

            continue;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);

        if (binding) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(dst, &out, 1000.0f, false));
            TEST_ASSERT_TRUE(dst->pts == received);

            /* the rows of each plane, packed as the direct run writes them */
            for (p = 0; p < 3u && dst->data[p] != NULL; p++) {
                h = (p == 0 || output->layout == 0) ? TF_H : TF_H / 2;
                row = (output->layout == 0) ? TF_W * 6 : ((p == 0) ? TF_W * 2 : TF_W);

                for (y = 0; y < h; y++) {
                    memcpy(rows + (size_t)received * frame_bytes +
                               ((p == 0) ? 0u
                                         : (size_t)TF_W * TF_H * 2u +
                                               (p - 1u) * (size_t)TF_W * TF_H / 2u) +
                               (size_t)y * (size_t)row,
                           dst->data[p] + y * dst->linesize[p], (size_t)row);
                }
            }

            av_frame_free(&dst);
        }

        received++;
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    av_buffer_pool_uninit(&pool);

    for (sent = 0; sent < TF_FRAMES; sent++) {
        av_frame_free(&kept[sent]);
    }
}

static void tf_check_case(const tf_case_t *c)
{
    hdrg_session_pixfmt_t input;
    hdrg_session_format_t raw;
    AVFrame *frame;

    frame = av_frame_alloc();
    TEST_ASSERT_NOT_NULL(frame);
    frame->format = c->format;
    frame->width = TF_W;
    frame->height = TF_H;
    TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));
    frame->color_range = (enum AVColorRange)c->range;
    frame->colorspace = (enum AVColorSpace)c->space;
    frame->color_primaries = (enum AVColorPrimaries)c->primaries;
    frame->color_trc = (enum AVColorTransferCharacteristic)c->trc;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(frame, &raw, &input));
    TEST_ASSERT_EQUAL_UINT32(TF_W, raw.width);
    TEST_ASSERT_EQUAL_UINT8(c->prim_out, raw.primaries);
    TEST_ASSERT_EQUAL_UINT8(c->trc_out, raw.transfer);
    TEST_ASSERT_EQUAL_UINT8(c->layout, input.layout);

    if (c->layout != 0) {
        TEST_ASSERT_EQUAL_UINT8(c->bits, input.bits);
        TEST_ASSERT_EQUAL_UINT8(c->matrix, input.matrix);
        TEST_ASSERT_TRUE(input.full_range == c->full);
    } else {
        TEST_ASSERT_EQUAL_UINT32(c->bits, raw.bit_depth);
        TEST_ASSERT_TRUE(raw.has_alpha == c->alpha);
        TEST_ASSERT_TRUE(raw.stride == (size_t)frame->linesize[0]);
    }

    av_frame_free(&frame);
}

/* the frame's chroma location (AVChromaLocation = H.273 + 1); unspecified: LEFT, yuvj*: CENTER */
static void test_chroma_location(void)
{
    static const uint8_t want[6] = {HDRG_CHROMA_LEFT,   HDRG_CHROMA_TOPLEFT, HDRG_CHROMA_BOTTOM,
                                    HDRG_CHROMA_CENTER, HDRG_CHROMA_CENTER,  HDRG_CHROMA_LEFT};
    static const int32_t formats[6] = {AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUV420P,
                                       AV_PIX_FMT_YUV420P,  AV_PIX_FMT_YUV422P10,
                                       AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ420P},
                         locations[6] = {AVCHROMA_LOC_UNSPECIFIED, AVCHROMA_LOC_TOPLEFT,
                                         AVCHROMA_LOC_BOTTOM,      AVCHROMA_LOC_CENTER,
                                         AVCHROMA_LOC_UNSPECIFIED, AVCHROMA_LOC_LEFT};
    hdrg_session_pixfmt_t input;
    hdrg_session_format_t raw;
    AVFrame *frame;
    uint8_t i;

    for (i = 0; i < 6u; i++) {
        frame = av_frame_alloc();
        TEST_ASSERT_NOT_NULL(frame);
        frame->format = formats[i];
        frame->width = TF_W;
        frame->height = TF_H;
        frame->chroma_location = (enum AVChromaLocation)locations[i];
        TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(frame, &raw, &input));
        TEST_ASSERT_EQUAL_UINT8(want[i], input.chroma_loc);

        av_frame_free(&frame);
    }
}

static void test_input_formats(void)
{
    const tf_case_t cases[] = {
        {AV_PIX_FMT_YUV420P, AVCOL_RANGE_UNSPECIFIED, AVCOL_SPC_UNSPECIFIED, AVCOL_PRI_UNSPECIFIED,
         AVCOL_TRC_UNSPECIFIED, HDRG_YUV_420, 8, HDRG_MATRIX_BT709, 1, 1, false, false},
        {AV_PIX_FMT_YUVJ444P, AVCOL_RANGE_UNSPECIFIED, AVCOL_SPC_BT709, AVCOL_PRI_BT709,
         AVCOL_TRC_BT709, HDRG_YUV_444, 8, HDRG_MATRIX_BT709, 1, 1, true, false},
        {AV_PIX_FMT_YUV422P10, AVCOL_RANGE_JPEG, AVCOL_SPC_BT470BG, AVCOL_PRI_BT470BG,
         AVCOL_TRC_SMPTE170M, HDRG_YUV_422, 10, HDRG_MATRIX_BT601, 5, 6, true, false},
        {AV_PIX_FMT_YUV420P12, AVCOL_RANGE_MPEG, AVCOL_SPC_UNSPECIFIED, AVCOL_PRI_BT2020,
         AVCOL_TRC_BT2020_10, HDRG_YUV_420, 12, HDRG_MATRIX_BT2020, 9, 14, false, false},
        {AV_PIX_FMT_RGB24, AVCOL_RANGE_UNSPECIFIED, AVCOL_SPC_RGB, AVCOL_PRI_UNSPECIFIED,
         AVCOL_TRC_UNSPECIFIED, 0, 8, 0, 1, 13, false, false},
        {AV_PIX_FMT_RGBA64, AVCOL_RANGE_JPEG, AVCOL_SPC_RGB, AVCOL_PRI_SMPTE432, AVCOL_TRC_GAMMA22,
         0, 16, 0, 12, 4, false, true},
    };
    const uint8_t layouts[5] = {HDRG_YUV_420, HDRG_YUV_420, HDRG_YUV_420, HDRG_YUV_422,
                                HDRG_YUV_422},
                  depths[5] = {8, 10, 16, 8, 10};
    const int32_t refused[] = {AV_PIX_FMT_NV21,  AV_PIX_FMT_NE(P010LE, P010BE),
                               AV_PIX_FMT_GBRP,  AV_PIX_FMT_YUVA420P,
                               AV_PIX_FMT_GRAY8, AV_PIX_FMT_NE(YUV420P10LE, YUV420P10BE)},
                  packed[5] = {AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_P016, AV_PIX_FMT_NV16,
                               AV_PIX_FMT_P210};
    const bool msbs[5] = {false, true, false, false, true};
    hdrg_session_pixfmt_t input;
    hdrg_session_format_t raw;
    AVFrame *frame;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        tf_check_case(&cases[i]);
    }

    /* Cb and Cr alternating in one plane, the bits in the low or high bits */
    for (i = 0; i < 5u; i++) {
        frame = av_frame_alloc();
        frame->format = packed[i];
        frame->width = TF_W;
        frame->height = TF_H;
        TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(frame, &raw, &input));
        TEST_ASSERT_EQUAL_UINT8(layouts[i], input.layout);
        TEST_ASSERT_EQUAL_UINT8(depths[i], input.bits);
        TEST_ASSERT_TRUE(input.interleaved);
        TEST_ASSERT_TRUE(input.msb == msbs[i]);
        TEST_ASSERT_EQUAL_UINT32(0, hdrg_ff_device(frame));
        av_frame_free(&frame);
    }

    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        frame = av_frame_alloc();
        frame->format = refused[i];
        frame->width = TF_W;
        frame->height = TF_H;
        TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, hdrg_ff_input(frame, &raw, &input));
        av_frame_free(&frame);
    }

    /* HDR sources are not graded again */
    frame = tf_frame(AV_PIX_FMT_YUV420P10, 0);
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, hdrg_ff_input(frame, &raw, &input));
    frame->color_trc = AVCOL_TRC_ARIB_STD_B67;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, hdrg_ff_input(frame, &raw, &input));
    av_frame_free(&frame);
}

/* the session frame is a reference to the frame's buffers, dropped on release */
static void test_zero_copy(void)
{
    hdrg_session_pixfmt_t input;
    hdrg_session_frame_t in;
    hdrg_session_format_t raw;
    AVFrame *frame;
    uint8_t p;

    frame = tf_frame(AV_PIX_FMT_YUV420P10, 3);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(frame, &raw, &input));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_frame(frame, &raw, &input, &in));

    for (p = 0; p < 3u; p++) {
        TEST_ASSERT_TRUE(in.planes[p] == frame->data[p]);
        TEST_ASSERT_TRUE(in.strides[p] == (size_t)frame->linesize[p]);
    }

    TEST_ASSERT_TRUE(in.pts == 3);
    TEST_ASSERT_EQUAL_INT(2, av_buffer_get_ref_count(frame->buf[0]));
    in.release(in.opaque);
    TEST_ASSERT_EQUAL_INT(1, av_buffer_get_ref_count(frame->buf[0]));
    av_frame_free(&frame);

    /* R'G'B' rows must be the format's stride apart */
    frame = tf_frame(AV_PIX_FMT_RGB24, 0);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(frame, &raw, &input));
    raw.stride += 16u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_ff_frame(frame, &raw, &input, &in));
    av_frame_free(&frame);
}

/*
 * AV1's film grain parameters on a frame (export_side_data film_grain) as
 * the session frame's HDRG_SD_FILM_GRAIN, its depth and range the frame's
 * where not given; H.274's and those out of the syntax's ranges left out
 */
static void test_film_grain(void)
{
    hdrg_session_pixfmt_t input;
    hdrg_session_frame_t in;
    hdrg_session_format_t raw;
    hdrg_sd_film_grain_t head;
    hdrg_sd_film_grain_av1_t av1;
    AVFilmGrainParams *params;
    AVFrame *frame;
    uint8_t i;

    frame = tf_frame(AV_PIX_FMT_YUV420P10, 0);
    frame->color_range = AVCOL_RANGE_MPEG;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(frame, &raw, &input));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_frame(frame, &raw, &input, &in));
    TEST_ASSERT_NULL(in.film_grain);
    TEST_ASSERT_TRUE(in.film_grain_size == 0);
    in.release(in.opaque);

    params = av_film_grain_params_create_side_data(frame);
    TEST_ASSERT_NOT_NULL(params);
    params->type = AV_FILM_GRAIN_PARAMS_AV1;
    params->codec.aom.num_y_points = 2;
    params->codec.aom.y_points[0][0] = 16;
    params->codec.aom.y_points[0][1] = 30;
    params->codec.aom.y_points[1][0] = 200;
    params->codec.aom.y_points[1][1] = 60;
    params->codec.aom.num_uv_points[1] = 1;
    params->codec.aom.uv_points[1][0][0] = 128;
    params->codec.aom.uv_points[1][0][1] = 20;
    params->codec.aom.scaling_shift = 10;
    params->codec.aom.ar_coeff_lag = 2;
    params->codec.aom.ar_coeffs_y[5] = -7;
    params->codec.aom.ar_coeffs_uv[1][12] = 33;
    params->codec.aom.ar_coeff_shift = 8;
    params->codec.aom.grain_scale_shift = 1;
    params->codec.aom.uv_mult[1] = -20;
    params->codec.aom.uv_mult_luma[1] = 64;
    params->codec.aom.uv_offset[1] = -100;
    params->codec.aom.overlap_flag = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_frame(frame, &raw, &input, &in));
    TEST_ASSERT_NOT_NULL(in.film_grain);
    TEST_ASSERT_TRUE(in.film_grain_size == sizeof(head) + sizeof(av1));
    memcpy(&head, in.film_grain, sizeof(head));
    memcpy(&av1, (const uint8_t *)in.film_grain + sizeof(head), sizeof(av1));
    TEST_ASSERT_EQUAL_UINT32(HDRG_FILM_GRAIN_AV1, head.kind);
    TEST_ASSERT_EQUAL_UINT32(10, head.bit_depth);
    TEST_ASSERT_EQUAL_UINT32(0, head.full_range);
    TEST_ASSERT_EQUAL_UINT32(1, head.subsampling_x);
    TEST_ASSERT_EQUAL_UINT32(1, head.subsampling_y);
    TEST_ASSERT_EQUAL_UINT8(2, av1.num_y_points);
    TEST_ASSERT_EQUAL_UINT8(60, av1.y_points[1][1]);
    TEST_ASSERT_EQUAL_UINT8(1, av1.num_uv_points[1]);
    TEST_ASSERT_EQUAL_UINT8(20, av1.uv_points[1][0][1]);
    TEST_ASSERT_EQUAL_UINT8(10, av1.scaling_shift);
    TEST_ASSERT_EQUAL_UINT8(2, av1.ar_coeff_lag);
    TEST_ASSERT_EQUAL_INT8(-7, av1.ar_coeffs_y[5]);
    TEST_ASSERT_EQUAL_INT8(33, av1.ar_coeffs_uv[1][12]);
    TEST_ASSERT_EQUAL_UINT8(8, av1.ar_coeff_shift);
    TEST_ASSERT_EQUAL_UINT8(1, av1.grain_scale_shift);
    TEST_ASSERT_EQUAL_INT16(-20, av1.uv_mult[1]);
    TEST_ASSERT_EQUAL_INT16(64, av1.uv_mult_luma[1]);
    TEST_ASSERT_EQUAL_INT16(-100, av1.uv_offset[1]);
    TEST_ASSERT_EQUAL_UINT8(1, av1.overlap);
    in.release(in.opaque);

    /* H.274's, a shift out of AV1's range */
    for (i = 0; i < 2u; i++) {
        params->type = (i == 0u) ? AV_FILM_GRAIN_PARAMS_H274 : AV_FILM_GRAIN_PARAMS_AV1;
        params->codec.aom.scaling_shift = (i == 0u) ? 10 : 12;
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_frame(frame, &raw, &input, &in));
        TEST_ASSERT_NULL(in.film_grain);
        in.release(in.opaque);
    }

    av_frame_free(&frame);
}

/* a frame of format and size (its pixels left as allocated) */
static AVFrame *tf_shaped(int32_t format, int32_t width, int32_t height)
{
    AVFrame *frame;

    frame = av_frame_alloc();
    TEST_ASSERT_NOT_NULL(frame);
    frame->format = format;
    frame->width = width;
    frame->height = height;
    TEST_ASSERT_EQUAL_INT(0, av_frame_get_buffer(frame, 0));

    return frame;
}

/*
 * A stream whose size or pixel format changes mid-way: a frame not of the
 * session's size and samples is refused (the session reads its format's rows
 * from every plane; a smaller frame of the same line size, or 4:2:0 chroma
 * planes under a 4:4:4 session, would be read past their end)
 */
static void test_frame_changes(void)
{
    static const int32_t shapes[4][3] = {
        {AV_PIX_FMT_YUV420P10, TF_W, TF_H / 2},
        {AV_PIX_FMT_YUV420P10, TF_W / 2, TF_H},
        {AV_PIX_FMT_YUV444P10, TF_W, TF_H},
        {AV_PIX_FMT_YUV420P, TF_W, TF_H},
    };
    hdrg_session_pixfmt_t input;
    hdrg_session_frame_t in;
    hdrg_session_format_t raw;
    AVFrame *first, *frame;
    uint8_t i;

    first = tf_frame(AV_PIX_FMT_YUV420P10, 0);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(first, &raw, &input));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_frame(first, &raw, &input, &in));
    in.release(in.opaque);

    for (i = 0; i < 4u; i++) {
        frame = tf_shaped(shapes[i][0], shapes[i][1], shapes[i][2]);
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_ff_frame(frame, &raw, &input, &in));
        av_frame_free(&frame);
    }

    av_frame_free(&first);

    /* R'G'B': another width or the alpha */
    first = tf_frame(AV_PIX_FMT_RGB24, 0);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_input(first, &raw, &input));
    frame = tf_shaped(AV_PIX_FMT_RGB24, TF_W, TF_H - 2);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_ff_frame(frame, &raw, &input, &in));
    av_frame_free(&frame);
    frame = tf_shaped(AV_PIX_FMT_RGBA, TF_W, TF_H);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_ff_frame(frame, &raw, &input, &in));
    av_frame_free(&frame);
    av_frame_free(&first);
}

/* the binding's frames and output frames change nothing: Y'CbCr and R'G'B' */
static void test_session_matches(void)
{
    const hdrg_session_pixfmt_t yuv = {HDRG_YUV_420, HDRG_MATRIX_BT2020,
                                       10,           HDRG_CHROMA_LEFT,
                                       false,        false,
                                       false},
                                rgb = {0, 0, 16, 0, false, false, false};
    const size_t frame_bytes = (size_t)TF_W * TF_H * 6u;
    uint8_t *direct, *bound;

    direct = (uint8_t *)calloc(TF_FRAMES, frame_bytes);
    bound = (uint8_t *)calloc(TF_FRAMES, frame_bytes);
    TEST_ASSERT_TRUE(direct != NULL && bound != NULL);

    tf_run(AV_PIX_FMT_YUV420P10, &yuv, false, direct, frame_bytes);
    tf_run(AV_PIX_FMT_YUV420P10, &yuv, true, bound, frame_bytes);
    TEST_ASSERT_EQUAL_MEMORY(direct, bound, frame_bytes * TF_FRAMES);

    /* the same samples as P010 */
    memset(bound, 0, frame_bytes * TF_FRAMES);
    tf_run(AV_PIX_FMT_P010, &yuv, true, bound, frame_bytes);
    TEST_ASSERT_EQUAL_MEMORY(direct, bound, frame_bytes * TF_FRAMES);

    memset(direct, 0, frame_bytes * TF_FRAMES);
    memset(bound, 0, frame_bytes * TF_FRAMES);
    tf_run(AV_PIX_FMT_RGB24, &rgb, false, direct, frame_bytes);
    tf_run(AV_PIX_FMT_RGB24, &rgb, true, bound, frame_bytes);
    TEST_ASSERT_EQUAL_MEMORY(direct, bound, frame_bytes * TF_FRAMES);

    free(direct);
    free(bound);
}

/* the output's colors and mastering display */
static void test_output_props(void)
{
    const hdrg_session_pixfmt_t yuv = {HDRG_YUV_420, HDRG_MATRIX_BT2020,
                                       10,           HDRG_CHROMA_LEFT,
                                       false,        false,
                                       false},
                                rgb = {0, 0, 16, 0, false, false, false},
                                bad = {HDRG_YUV_444, 9, 10, 0, false, false, false};
    const AVMasteringDisplayMetadata *master;
    const AVFrameSideData *sd;
    hdrg_session_output_t out;
    AVFrame *frame;

    TEST_ASSERT_EQUAL_INT(AV_PIX_FMT_YUV420P10, hdrg_ff_output_format(&yuv));
    TEST_ASSERT_EQUAL_INT(AV_PIX_FMT_RGB48, hdrg_ff_output_format(&rgb));
    TEST_ASSERT_EQUAL_INT(AV_PIX_FMT_NONE, hdrg_ff_output_format(&bad));
    TEST_ASSERT_NULL(hdrg_ff_output_frame(TF_W, TF_H, &bad, NULL, &out));

    frame = hdrg_ff_output_frame(TF_W, TF_H, &yuv, NULL, &out);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE(out.planes[0] == frame->data[0] && out.planes[2] == frame->data[2]);
    out.pts = 42;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 812.5f, false));
    TEST_ASSERT_TRUE(frame->pts == 42);
    TEST_ASSERT_EQUAL_INT(AVCOL_TRC_SMPTE2084, frame->color_trc);
    TEST_ASSERT_EQUAL_INT(AVCOL_PRI_BT2020, frame->color_primaries);
    TEST_ASSERT_EQUAL_INT(AVCOL_SPC_BT2020_NCL, frame->colorspace);
    TEST_ASSERT_EQUAL_INT(AVCOL_RANGE_MPEG, frame->color_range);
    TEST_ASSERT_EQUAL_INT(AVCHROMA_LOC_LEFT, frame->chroma_location);

    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    TEST_ASSERT_NOT_NULL(sd);
    master = (const AVMasteringDisplayMetadata *)(const void *)sd->data;
    TEST_ASSERT_TRUE(master->has_primaries && master->has_luminance);
    TEST_ASSERT_EQUAL_INT(34000, master->display_primaries[0][0].num);
    TEST_ASSERT_EQUAL_INT(50000, master->display_primaries[0][0].den);
    TEST_ASSERT_EQUAL_INT(15635, master->white_point[0].num);
    TEST_ASSERT_EQUAL_INT(8125000, master->max_luminance.num);
    TEST_ASSERT_EQUAL_INT(50, master->min_luminance.num);

    /* finishing again keeps one mastering display */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 812.5f, false));
    TEST_ASSERT_EQUAL_INT(1, frame->nb_side_data);
    av_frame_free(&frame);

    frame = hdrg_ff_output_frame(TF_W, TF_H, &rgb, NULL, &out);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 812.5f, false));
    TEST_ASSERT_EQUAL_INT(AVCOL_PRI_SMPTE432, frame->color_primaries);
    TEST_ASSERT_EQUAL_INT(AVCOL_SPC_RGB, frame->colorspace);
    TEST_ASSERT_EQUAL_INT(AVCOL_RANGE_JPEG, frame->color_range);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 812.5f, true));
    TEST_ASSERT_EQUAL_INT(AVCOL_PRI_BT2020, frame->color_primaries);
    av_frame_free(&frame);
}

/*
 * frames from a pool: rows and planes aligned to 64 bytes, a buffer still
 * referenced not given again and given again once it is not, a larger
 * frame than the pool's refused
 */
static void test_pool_frame(void)
{
    const uint8_t *first;
    AVBufferPool *pool;
    AVFrame *a, *b, *c, *ref;
    uint8_t p;

    pool = NULL;
    TEST_ASSERT_NULL(hdrg_ff_pool_frame(NULL, TF_W, TF_H, AV_PIX_FMT_YUV420P10));
    a = hdrg_ff_pool_frame(&pool, TF_W, TF_H, AV_PIX_FMT_YUV420P10);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_TRUE(av_frame_is_writable(a));
    TEST_ASSERT_TRUE(a->width == TF_W && a->height == TF_H && a->format == AV_PIX_FMT_YUV420P10);

    for (p = 0; p < 3u; p++) {
        TEST_ASSERT_NOT_NULL(a->data[p]);
        TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)((uintptr_t)a->data[p] % 64u));
        TEST_ASSERT_EQUAL_INT(0, a->linesize[p] % 64);
        TEST_ASSERT_TRUE(a->linesize[p] >= ((p == 0) ? TF_W * 2 : TF_W));
    }

    TEST_ASSERT_TRUE(a->data[1] >= a->data[0] + (size_t)a->linesize[0] * TF_H);
    TEST_ASSERT_TRUE(a->data[2] >= a->data[1] + (size_t)a->linesize[1] * (TF_H / 2));

    first = a->data[0];
    ref = av_frame_clone(a);
    av_frame_free(&a);
    b = hdrg_ff_pool_frame(&pool, TF_W, TF_H, AV_PIX_FMT_YUV420P10);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(b->data[0] != first);
    av_frame_free(&ref);
    c = hdrg_ff_pool_frame(&pool, TF_W, TF_H, AV_PIX_FMT_YUV420P10);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_TRUE(c->data[0] == first);

    TEST_ASSERT_NULL(hdrg_ff_pool_frame(&pool, TF_W * 2, TF_H * 2, AV_PIX_FMT_YUV420P10));
    av_frame_free(&b);
    av_frame_free(&c);
    av_buffer_pool_uninit(&pool);
}

/*
 * the output's HDR10+ on the frame: one window, cd/m² over 10000 in 1/100000
 * (held at 1), the percentiles at their percentages; FFmpeg serializes it as
 * ITU-T T.35 into hdrg_hdr10plus_t35's bytes; finished again, one of it; none
 * measured, none
 */
static void test_hdr10plus(void)
{
    const hdrg_session_pixfmt_t yuv = {
        HDRG_YUV_420, HDRG_MATRIX_BT2020, 10, HDRG_CHROMA_LEFT, false, false, false};
    const AVHDRPlusColorTransformParams *window;
    const AVDynamicHDRPlus *plus;
    const AVFrameSideData *sd;
    hdrg_session_output_t out;
    AVFrame *frame;
    uint8_t i, *t35, ours[HDRG_HDR10PLUS_T35_BYTES];
    size_t size;

    frame = hdrg_ff_output_frame(TF_W, TF_H, &yuv, NULL, &out);
    TEST_ASSERT_NOT_NULL(frame);
    out.hdr10plus.valid = true;
    out.hdr10plus.maxscl[0] = 812.5f;
    out.hdr10plus.maxscl[1] = 400.0f;
    out.hdr10plus.maxscl[2] = 20000.0f;
    out.hdr10plus.average_maxrgb = 50.0f;

    for (i = 0; i < HDRG_HDR10PLUS_PERCENTILES; i++) {
        out.hdr10plus.percentiles[i] = 1.0f + 100.0f * (float)i;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 812.5f, false));
    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    TEST_ASSERT_NOT_NULL(sd);
    plus = (const AVDynamicHDRPlus *)(const void *)sd->data;
    window = &plus->params[0];
    TEST_ASSERT_EQUAL_INT(0xB5, plus->itu_t_t35_country_code);
    TEST_ASSERT_EQUAL_INT(1, plus->application_version);
    TEST_ASSERT_EQUAL_INT(1, plus->num_windows);
    TEST_ASSERT_EQUAL_INT(8125, window->maxscl[0].num);
    TEST_ASSERT_EQUAL_INT(100000, window->maxscl[0].den);
    TEST_ASSERT_EQUAL_INT(100000, window->maxscl[2].num);
    TEST_ASSERT_EQUAL_INT(500, window->average_maxrgb.num);
    TEST_ASSERT_EQUAL_INT(HDRG_HDR10PLUS_PERCENTILES, window->num_distribution_maxrgb_percentiles);

    for (i = 0; i < HDRG_HDR10PLUS_PERCENTILES; i++) {
        TEST_ASSERT_EQUAL_INT(tf_percentages[i], window->distribution_maxrgb[i].percentage);
        TEST_ASSERT_EQUAL_INT(10 + 1000 * i, window->distribution_maxrgb[i].percentile.num);
    }

    TEST_ASSERT_EQUAL_INT(0, window->tone_mapping_flag);
    t35 = NULL;
    size = 0;
    TEST_ASSERT_TRUE(av_dynamic_hdr_plus_to_t35(plus, &t35, &size) >= 0);
    TEST_ASSERT_EQUAL_UINT32(HDRG_HDR10PLUS_T35_BYTES,
                             (uint32_t)hdrg_hdr10plus_t35(&out.hdr10plus, ours, sizeof(ours)));
    TEST_ASSERT_EQUAL_UINT32(HDRG_HDR10PLUS_T35_BYTES, (uint32_t)size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ours, t35, HDRG_HDR10PLUS_T35_BYTES);
    av_free(t35);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 812.5f, false));
    TEST_ASSERT_EQUAL_INT(2, frame->nb_side_data);
    out.hdr10plus.valid = false;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 812.5f, false));
    TEST_ASSERT_EQUAL_INT(1, frame->nb_side_data);
    TEST_ASSERT_NULL(av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS));
    av_frame_free(&frame);
}

/*
 * the output's ST 2094-50 on the frame (FFmpeg 9 and later): one alternate
 * at headroom 0, its points in ST 2094-50's units (rising x, |y|), the
 * reference white custom when not 203; FFmpeg serializes it as ITU-T T.35
 * into hdrg_st2094_50_t35's bytes and reads them back the same; none fitted,
 * none
 */
static void test_st2094_50(void)
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
    const hdrg_session_pixfmt_t yuv = {
        HDRG_YUV_420, HDRG_MATRIX_BT2020, 10, HDRG_CHROMA_LEFT, false, false, false};
    const AVDynamicHDRSmpte2094App5 *app5;
    const AVFrameSideData *sd;
    AVDynamicHDRSmpte2094App5 *back;
    hdrg_session_output_t out;
    AVFrame *frame;
    uint8_t i, *t35, ours[HDRG_ST2094_50_T35_MAX];
    size_t size;

    frame = hdrg_ff_output_frame(TF_W, TF_H, &yuv, NULL, &out);
    TEST_ASSERT_NOT_NULL(frame);
    out.st2094_50.valid = true;
    out.st2094_50.baseline_headroom = 2.3f;
    out.st2094_50.reference_white = 203.0f;
    out.st2094_50.gamut = HDRG_GAMUT_BT2020;
    out.st2094_50.count = 3;
    out.st2094_50.x[0] = 0.01f;
    out.st2094_50.x[1] = 1.0f;
    out.st2094_50.x[2] = 4.0f;
    out.st2094_50.y[0] = -0.1f;
    out.st2094_50.y[1] = -1.0f;
    out.st2094_50.y[2] = -2.25f;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 1000.0f, false));
    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_SMPTE_2094_APP5);
    TEST_ASSERT_NOT_NULL(sd);
    app5 = (const AVDynamicHDRSmpte2094App5 *)(const void *)sd->data;
    TEST_ASSERT_EQUAL_INT(1, app5->has_adaptive_tone_map_flag);
    TEST_ASSERT_EQUAL_INT(0, app5->has_custom_hdr_reference_white_flag);
    TEST_ASSERT_EQUAL_INT(23000, app5->baseline_hdr_headroom);
    TEST_ASSERT_EQUAL_INT(1, app5->num_alternate_images);
    TEST_ASSERT_EQUAL_INT(2, app5->gain_application_space_chromaticities_flag);
    TEST_ASSERT_EQUAL_INT(0, app5->alternate_hdr_headrooms[0]);
    TEST_ASSERT_EQUAL_INT(2, app5->gain_curve_num_control_points_minus_1[0]);
    TEST_ASSERT_EQUAL_INT(1, app5->gain_curve_use_pchip_slope_flag[0]);
    TEST_ASSERT_EQUAL_INT(10, app5->gain_curve_control_points_x[0][0]);
    TEST_ASSERT_EQUAL_INT(4000, app5->gain_curve_control_points_x[0][2]);
    TEST_ASSERT_EQUAL_INT(1000, app5->gain_curve_control_points_y[0][0]);
    TEST_ASSERT_EQUAL_INT(22500, app5->gain_curve_control_points_y[0][2]);

    t35 = NULL;
    size = 0;
    TEST_ASSERT_TRUE(av_dynamic_hdr_smpte2094_app5_to_t35(app5, &t35, &size) >= 0);
    TEST_ASSERT_EQUAL_UINT32(size,
                             (uint32_t)hdrg_st2094_50_t35(&out.st2094_50, ours, sizeof(ours)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ours, t35, size);
    back = av_dynamic_hdr_smpte2094_app5_alloc(NULL);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_TRUE(av_dynamic_hdr_smpte2094_app5_from_t35(back, t35, size) >= 0);
    TEST_ASSERT_EQUAL_INT(app5->baseline_hdr_headroom, back->baseline_hdr_headroom);

    for (i = 0; i < 3u; i++) {
        TEST_ASSERT_EQUAL_INT(app5->gain_curve_control_points_x[0][i],
                              back->gain_curve_control_points_x[0][i]);
        TEST_ASSERT_EQUAL_INT(app5->gain_curve_control_points_y[0][i],
                              back->gain_curve_control_points_y[0][i]);
    }

    av_free(t35);
    av_free(back);

    out.st2094_50.reference_white = 100.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 1000.0f, false));
    sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_SMPTE_2094_APP5);
    app5 = (const AVDynamicHDRSmpte2094App5 *)(const void *)sd->data;
    TEST_ASSERT_EQUAL_INT(1, app5->has_custom_hdr_reference_white_flag);
    TEST_ASSERT_EQUAL_INT(500, app5->hdr_reference_white);
    t35 = NULL;
    size = 0;
    TEST_ASSERT_TRUE(av_dynamic_hdr_smpte2094_app5_to_t35(app5, &t35, &size) >= 0);
    TEST_ASSERT_EQUAL_UINT32(size,
                             (uint32_t)hdrg_st2094_50_t35(&out.st2094_50, ours, sizeof(ours)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ours, t35, size);
    av_free(t35);

    out.st2094_50.valid = false;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_finish(frame, &out, 1000.0f, false));
    TEST_ASSERT_NULL(av_frame_get_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_SMPTE_2094_APP5));
    av_frame_free(&frame);
#else
    TEST_IGNORE_MESSAGE("FFmpeg before 9: no ST 2094-50 side data");
#endif
}

/* an output keeps the source's captions and timecodes, not its SDR or coding descriptions */
static void test_side_data_policy(void)
{
    const enum AVFrameSideDataType dropped[] = {AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                                                AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
                                                AV_FRAME_DATA_ICC_PROFILE,
                                                AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
                                                AV_FRAME_DATA_FILM_GRAIN_PARAMS},
                                   kept[] = {AV_FRAME_DATA_A53_CC, AV_FRAME_DATA_S12M_TIMECODE,
                                             AV_FRAME_DATA_SEI_UNREGISTERED};
    AVFrame *src, *dst;
    size_t i;

    src = tf_frame(AV_PIX_FMT_YUV420P10, 5);
    dst = av_frame_alloc();
    TEST_ASSERT_NOT_NULL(dst);

    for (i = 0; i < sizeof(dropped) / sizeof(dropped[0]); i++) {
        TEST_ASSERT_NOT_NULL(av_frame_new_side_data(src, dropped[i], 64));
        TEST_ASSERT_FALSE(hdrg_ff_keeps(dropped[i]));
    }

    for (i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        TEST_ASSERT_NOT_NULL(av_frame_new_side_data(src, kept[i], 16));
        TEST_ASSERT_TRUE(hdrg_ff_keeps(kept[i]));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_copy_props(dst, src));
    TEST_ASSERT_TRUE(dst->pts == 5);

    for (i = 0; i < sizeof(dropped) / sizeof(dropped[0]); i++) {
        TEST_ASSERT_NULL(av_frame_get_side_data(dst, dropped[i]));
    }

    for (i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        TEST_ASSERT_NOT_NULL(av_frame_get_side_data(dst, kept[i]));
    }

    av_frame_free(&src);
    av_frame_free(&dst);
}

/* an encoder's side data: the mastering display, the content light level when known */
static void test_encoder_side_data(void)
{
    const AVContentLightMetadata *light;
    const AVFrameSideData *found;
    AVFrameSideData **sd;
    int count;

    sd = NULL;
    count = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_hdr_side_data(&sd, &count, 1000.0f, 0.0f, 0.0f));
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_INT(AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, sd[0]->type);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_ff_hdr_side_data(&sd, &count, 1000.0f, 950.4f, 210.6f));
    TEST_ASSERT_EQUAL_INT(2, count);
    found = av_frame_side_data_get(sd, count, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    TEST_ASSERT_NOT_NULL(found);
    light = (const AVContentLightMetadata *)(const void *)found->data;
    TEST_ASSERT_EQUAL_UINT32(950, light->MaxCLL);
    TEST_ASSERT_EQUAL_UINT32(211, light->MaxFALL);

    av_frame_side_data_free(&sd, &count);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_ff_hdr_side_data(NULL, &count, 1.0f, 0, 0));
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_input_formats);
    RUN_TEST(test_chroma_location);
    RUN_TEST(test_zero_copy);
    RUN_TEST(test_film_grain);
    RUN_TEST(test_frame_changes);
    RUN_TEST(test_session_matches);
    RUN_TEST(test_output_props);
    RUN_TEST(test_pool_frame);
    RUN_TEST(test_hdr10plus);
    RUN_TEST(test_st2094_50);
    RUN_TEST(test_side_data_policy);
    RUN_TEST(test_encoder_side_data);

    return UNITY_END();
}
