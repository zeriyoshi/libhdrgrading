/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include "hdrg_ffmpeg.h"

#include <stdint.h>
#include <string.h>

#include <libavutil/buffer.h>
#include <libavutil/film_grain_params.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/macros.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/version.h>

#define HDRG_FF_CHROMA    50000 /* the denominators of the chromaticities and luminances */
#define HDRG_FF_LUMINANCE 10000
#define HDRG_FF_DROPPED   (sizeof(hdrg_ff_dropped) / sizeof(hdrg_ff_dropped[0]))
#define HDRG_FF_ALIGN     64 /* a pooled frame's rows and planes (FFmpeg's widest SIMD's) */

/* the side data of the SDR signal or its coding: an output does not keep it */
static const enum AVFrameSideDataType hdrg_ff_dropped[] = {
    AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
    AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
    AV_FRAME_DATA_ICC_PROFILE,
    AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
    AV_FRAME_DATA_DYNAMIC_HDR_VIVID,
    AV_FRAME_DATA_DOVI_RPU_BUFFER,
    AV_FRAME_DATA_DOVI_METADATA,
    AV_FRAME_DATA_AMBIENT_VIEWING_ENVIRONMENT,
    AV_FRAME_DATA_FILM_GRAIN_PARAMS,
    AV_FRAME_DATA_VIDEO_ENC_PARAMS,
};

/* the mastering display: Display P3 primaries (r, g, b) and its D65 white */
static const double hdrg_ff_p3[4][2] = {
    {0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}};

/* a session frame's reference to the AVFrame and, on the GPU, its pixel buffer; its film grain */
typedef struct {
    AVFrame *ref;
    hdrg_device_frame_t device;
    uint8_t film[sizeof(hdrg_sd_film_grain_t) + sizeof(hdrg_sd_film_grain_av1_t)];
} hdrg_ff_held_t;

static inline AVRational hdrg_ff_q(double value, int den)
{
    return av_make_q((int)(value * (double)den + 0.5), den);
}

/* a frame's pixels in memory: its own, or a hardware frame's sw_format; NULL: none */
static const AVPixFmtDescriptor *hdrg_ff_pixels(const AVFrame *frame)
{
    const AVHWFramesContext *frames;

    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX && frame->hw_frames_ctx != NULL) {
        frames = (const AVHWFramesContext *)(const void *)frame->hw_frames_ctx->data;

        return av_pix_fmt_desc_get(frames->sw_format);
    }

    return av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
}

static void hdrg_ff_release(void *opaque)
{
    hdrg_ff_held_t *held = (hdrg_ff_held_t *)opaque;

    av_frame_free(&held->ref);
    av_free(held);
}

/*
 * The frame's AV1 film grain its decoder left out (export_side_data
 * film_grain) as HDRG_SD_FILM_GRAIN into film; false: none (H.274's, the
 * syntax's ranges broken)
 */
static bool hdrg_ff_film(const AVFrame *frame, uint8_t *film)
{
    const AVFrameSideData *sd = av_frame_get_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS);
    const AVPixFmtDescriptor *desc = hdrg_ff_pixels(frame);
    const AVFilmGrainParams *params;
    const AVFilmGrainAOMParams *aom;
    hdrg_sd_film_grain_t head;
    hdrg_sd_film_grain_av1_t av1;
    uint8_t c;
    bool ok;

    if (sd == NULL || desc == NULL || sd->size < sizeof(AVFilmGrainParams)) {
        return false;
    }

    params = (const AVFilmGrainParams *)(const void *)sd->data;
    aom = &params->codec.aom;

    if (params->type != AV_FILM_GRAIN_PARAMS_AV1) {
        return false;
    }

    memset(&head, 0, sizeof(head));
    memset(&av1, 0, sizeof(av1));
    head.kind = HDRG_FILM_GRAIN_AV1;
    head.bit_depth =
        (uint32_t)((params->bit_depth_luma > 0) ? params->bit_depth_luma : desc->comp[0].depth);
    head.full_range = (((params->color_range != AVCOL_RANGE_UNSPECIFIED)
                            ? params->color_range
                            : frame->color_range) == AVCOL_RANGE_JPEG)
                          ? 1u
                          : 0u;
    head.subsampling_x =
        (params->width > 0) ? (uint32_t)params->subsampling_x : (uint32_t)desc->log2_chroma_w;
    head.subsampling_y =
        (params->width > 0) ? (uint32_t)params->subsampling_y : (uint32_t)desc->log2_chroma_h;

    ok = head.bit_depth >= 8u && head.bit_depth <= 12u && head.subsampling_x <= 1u &&
         head.subsampling_y <= head.subsampling_x && aom->num_y_points >= 0 &&
         aom->num_y_points <= 14 && aom->chroma_scaling_from_luma >= 0 &&
         aom->chroma_scaling_from_luma <= 1 && aom->scaling_shift >= 8 &&
         aom->scaling_shift <= 11 && aom->ar_coeff_lag >= 0 && aom->ar_coeff_lag <= 3 &&
         aom->ar_coeff_shift >= 6 && aom->ar_coeff_shift <= 9 && aom->grain_scale_shift >= 0 &&
         aom->grain_scale_shift <= 3;

    for (c = 0; ok && c < 2u; c++) {
        ok = aom->num_uv_points[c] >= 0 && aom->num_uv_points[c] <= 10 && aom->uv_mult[c] >= -128 &&
             aom->uv_mult[c] <= 127 && aom->uv_mult_luma[c] >= -128 &&
             aom->uv_mult_luma[c] <= 127 && aom->uv_offset[c] >= -256 && aom->uv_offset[c] <= 255;
    }

    if (!ok) {
        return false;
    }

    memcpy(av1.y_points, aom->y_points, sizeof(av1.y_points));
    memcpy(av1.uv_points, aom->uv_points, sizeof(av1.uv_points));
    memcpy(av1.ar_coeffs_y, aom->ar_coeffs_y, sizeof(av1.ar_coeffs_y));
    memcpy(av1.ar_coeffs_uv, aom->ar_coeffs_uv, sizeof(av1.ar_coeffs_uv));
    av1.num_y_points = (uint8_t)aom->num_y_points;
    av1.chroma_scaling_from_luma = (uint8_t)aom->chroma_scaling_from_luma;
    av1.scaling_shift = (uint8_t)aom->scaling_shift;
    av1.ar_coeff_lag = (uint8_t)aom->ar_coeff_lag;
    av1.ar_coeff_shift = (uint8_t)aom->ar_coeff_shift;
    av1.grain_scale_shift = (uint8_t)aom->grain_scale_shift;
    av1.overlap = (aom->overlap_flag != 0) ? 1u : 0u;

    for (c = 0; c < 2u; c++) {
        av1.num_uv_points[c] = (uint8_t)aom->num_uv_points[c];
        av1.uv_mult[c] = (int16_t)aom->uv_mult[c];
        av1.uv_mult_luma[c] = (int16_t)aom->uv_mult_luma[c];
        av1.uv_offset[c] = (int16_t)aom->uv_offset[c];
    }

    memcpy(film, &head, sizeof(head));
    memcpy(film + sizeof(head), &av1, sizeof(av1));

    return true;
}

/*
 * The output's ST 2094-50 on frame (FFmpeg 9: libavutil 60.30.100 and later;
 * before it, none): its T.35 (hdrg_st2094_50_t35: one alternate at headroom
 * 0, a max(R, G, B) gain curve of PCHIP slopes) as FFmpeg reads it
 */
static hdrg_status_t hdrg_ff_st2094_50(AVFrame *frame, const hdrg_st2094_50_t *curve)
{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
    AVDynamicHDRSmpte2094App5 *app5;
    uint8_t t35[HDRG_ST2094_50_T35_MAX];
    size_t size;

    av_frame_remove_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_SMPTE_2094_APP5);

    size = hdrg_st2094_50_t35(curve, t35, sizeof(t35));
    if (size == 0) {
        return HDRG_OK;
    }

    app5 = av_dynamic_hdr_smpte2094_app5_create_side_data(frame);
    if (app5 == NULL) {
        return HDRG_ERR_NOMEM;
    }

    if (av_dynamic_hdr_smpte2094_app5_from_t35(app5, t35, size) < 0) {
        return HDRG_ERR_INTERNAL;
    }
#else
    (void)frame;
    (void)curve;
#endif

    return HDRG_OK;
}

/* packed R'G'B': its sample bits and alpha; false for other formats */
static bool hdrg_ff_rgb(int32_t format, uint8_t *bits, bool *alpha)
{
    switch (format) {
    case AV_PIX_FMT_RGB24:
    case AV_PIX_FMT_RGBA:
        *bits = 8;
        *alpha = format == AV_PIX_FMT_RGBA;

        return true;
    case AV_PIX_FMT_RGB48:
    case AV_PIX_FMT_RGBA64:
        *bits = 16;
        *alpha = format == AV_PIX_FMT_RGBA64;

        return true;
    default:
        return false;
    }
}

/*
 * planar Y'CbCr of the host's byte order, one component a plane or Cb and Cr
 * alternating in the second (NV12, P010), the bits in the samples' low or
 * high bits: its pixels' layout, bits, interleaving and alignment
 */
static bool hdrg_ff_yuv(const AVPixFmtDescriptor *desc, hdrg_session_pixfmt_t *pixels)
{
    const uint64_t refused = AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                             AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_FLOAT |
                             AV_PIX_FMT_FLAG_BAYER;
    const uint16_t probe = 1;
    uint8_t c, step, shift;
    bool big, interleaved;

    if (desc == NULL || desc->nb_components != 3 || (desc->flags & refused) != 0 ||
        (desc->flags & AV_PIX_FMT_FLAG_PLANAR) == 0 || desc->comp[0].depth < 8 ||
        desc->comp[0].depth > 16) {
        return false;
    }

    big = *(const uint8_t *)&probe == 0;
    step = (desc->comp[0].depth > 8) ? 2u : 1u;
    shift = (uint8_t)desc->comp[0].shift;
    interleaved = desc->comp[1].plane == 1 && desc->comp[2].plane == 1;

    if ((step == 2u && ((desc->flags & AV_PIX_FMT_FLAG_BE) != 0) != big) ||
        (shift != 0u && (step != 2u || shift != 16u - (uint32_t)desc->comp[0].depth))) {
        return false;
    }

    for (c = 0; c < 3u; c++) {
        if (desc->comp[c].plane != (int)((interleaved && c == 2u) ? 1u : c) ||
            desc->comp[c].offset != (int)((interleaved && c == 2u) ? step : 0u) ||
            desc->comp[c].shift != (int)shift ||
            desc->comp[c].step != (int)((interleaved && c != 0u) ? 2u * step : step) ||
            desc->comp[c].depth != desc->comp[0].depth) {
            return false;
        }
    }

    pixels->interleaved = interleaved;
    pixels->msb = shift != 0u;

    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
        pixels->layout = HDRG_YUV_420;
    } else if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
        pixels->layout = HDRG_YUV_422;
    } else if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0) {
        pixels->layout = HDRG_YUV_444;
    } else {
        return false;
    }

    pixels->bits = (uint8_t)desc->comp[0].depth;

    return true;
}

static void hdrg_ff_master(AVMasteringDisplayMetadata *master, float peak_nits)
{
    uint8_t i;

    memset(master, 0, sizeof(*master));

    for (i = 0; i < 3u; i++) {
        master->display_primaries[i][0] = hdrg_ff_q(hdrg_ff_p3[i][0], HDRG_FF_CHROMA);
        master->display_primaries[i][1] = hdrg_ff_q(hdrg_ff_p3[i][1], HDRG_FF_CHROMA);
    }

    master->white_point[0] = hdrg_ff_q(hdrg_ff_p3[3][0], HDRG_FF_CHROMA);
    master->white_point[1] = hdrg_ff_q(hdrg_ff_p3[3][1], HDRG_FF_CHROMA);
    master->min_luminance = av_make_q(50, HDRG_FF_LUMINANCE);
    master->max_luminance = hdrg_ff_q((double)peak_nits, HDRG_FF_LUMINANCE);
    master->has_primaries = 1;
    master->has_luminance = 1;
}

/*
 * A pooled frame's planes: their rows' bytes, their offsets from the
 * buffer's aligned start, and the buffer's bytes (the start's slack, the
 * height padded to 32 rows and each plane's padding as av_frame_get_buffer's
 * for the decoders' and encoders' SIMD); false when it has none
 */
static bool hdrg_ff_layout(int width, int height, enum AVPixelFormat format, int *lines,
                           size_t *offsets, size_t *bytes)
{
    uint8_t p;
    ptrdiff_t pitches[4];
    size_t sizes[4];

    if (width <= 0 || height <= 0 || width > INT32_MAX - HDRG_FF_ALIGN || height > INT32_MAX - 32 ||
        av_image_fill_linesizes(lines, format, FFALIGN(width, HDRG_FF_ALIGN)) < 0) {
        return false;
    }

    for (p = 0; p < 4u; p++) {
        if (lines[p] < 0 || lines[p] > INT32_MAX - HDRG_FF_ALIGN) {
            return false;
        }

        lines[p] = FFALIGN(lines[p], HDRG_FF_ALIGN);
        pitches[p] = lines[p];
    }

    if (av_image_fill_plane_sizes(sizes, format, FFALIGN(height, 32), pitches) < 0) {
        return false;
    }

    *bytes = (size_t)HDRG_FF_ALIGN;

    for (p = 0; p < 4u; p++) {
        offsets[p] = *bytes - (size_t)HDRG_FF_ALIGN;

        if (sizes[p] > SIZE_MAX / 8u - *bytes) {
            return false;
        }

        *bytes +=
            (sizes[p] > 0) ? FFALIGN(sizes[p], (size_t)HDRG_FF_ALIGN) + (size_t)HDRG_FF_ALIGN : 0u;
    }

    return true;
}

extern hdrg_status_t hdrg_ff_input(const AVFrame *frame, hdrg_session_format_t *format,
                                   hdrg_session_pixfmt_t *input)
{
    const AVPixFmtDescriptor *desc;
    hdrg_session_pixfmt_t pixels;
    uint8_t bits;
    int32_t primaries, transfer, matrix;
    bool yuv, alpha;

    if (frame == NULL || format == NULL || input == NULL || frame->width <= 0 ||
        frame->height <= 0) {
        return HDRG_ERR_INVALID_ARG;
    }

    desc = hdrg_ff_pixels(frame);
    memset(&pixels, 0, sizeof(pixels));
    yuv = hdrg_ff_yuv(desc, &pixels);
    alpha = false;
    bits = 0;

    if (!yuv &&
        (frame->format == AV_PIX_FMT_VIDEOTOOLBOX || !hdrg_ff_rgb(frame->format, &bits, &alpha))) {
        return HDRG_ERR_UNSUPPORTED;
    }

    if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67) {
        return HDRG_ERR_UNSUPPORTED;
    }

    primaries = (frame->color_primaries == AVCOL_PRI_UNSPECIFIED) ? AVCOL_PRI_BT709
                                                                  : (int32_t)frame->color_primaries;
    transfer = (int32_t)frame->color_trc;

    if (transfer == AVCOL_TRC_UNSPECIFIED) {
        transfer = yuv ? AVCOL_TRC_BT709 : AVCOL_TRC_IEC61966_2_1;
    }

    if (primaries > 255 || transfer > 255) {
        return HDRG_ERR_UNSUPPORTED;
    }

    memset(format, 0, sizeof(*format));
    memset(input, 0, sizeof(*input));
    format->width = (uint32_t)frame->width;
    format->height = (uint32_t)frame->height;
    format->primaries = (uint8_t)primaries;
    format->transfer = (uint8_t)transfer;

    if (!yuv) {
        if (frame->linesize[0] <= 0) {
            return HDRG_ERR_INVALID_ARG;
        }

        format->bit_depth = bits;
        format->has_alpha = alpha;
        format->stride = (size_t)frame->linesize[0];

        return HDRG_OK;
    }

    matrix = (int32_t)frame->colorspace;

    if (matrix == AVCOL_SPC_UNSPECIFIED) {
        matrix = (primaries == AVCOL_PRI_BT2020) ? AVCOL_SPC_BT2020_NCL
                 : (primaries == AVCOL_PRI_BT470BG || primaries == AVCOL_PRI_SMPTE170M)
                     ? AVCOL_SPC_SMPTE170M
                     : AVCOL_SPC_BT709;
    }

    /* BT.470 BG and SMPTE 170M share the matrix */
    matrix = (matrix == AVCOL_SPC_BT470BG) ? AVCOL_SPC_SMPTE170M : matrix;

    if (matrix > 255) {
        return HDRG_ERR_UNSUPPORTED;
    }

    *input = pixels;
    input->matrix = (uint8_t)matrix;
    input->full_range =
        frame->color_range == AVCOL_RANGE_JPEG || strncmp(desc->name, "yuvj", 4) == 0;
    input->chroma_loc =
        (strncmp(desc->name, "yuvj", 4) == 0) ? HDRG_CHROMA_CENTER : HDRG_CHROMA_LEFT;

    /* AVChromaLocation is H.273's chroma_sample_loc_type + 1 */
    if (frame->chroma_location > AVCHROMA_LOC_UNSPECIFIED &&
        frame->chroma_location <= AVCHROMA_LOC_BOTTOM) {
        input->chroma_loc = (uint8_t)(frame->chroma_location - 1);
    }

    return HDRG_OK;
}

extern hdrg_status_t hdrg_ff_frame(const AVFrame *frame, const hdrg_session_format_t *format,
                                   const hdrg_session_pixfmt_t *input, hdrg_session_frame_t *out)
{
    hdrg_session_format_t own;
    hdrg_session_pixfmt_t own_input;
    hdrg_ff_held_t *held;
    uint8_t p;
    bool device;

    if (frame == NULL || format == NULL || input == NULL || out == NULL ||
        hdrg_ff_input(frame, &own, &own_input) != HDRG_OK) {
        return HDRG_ERR_INVALID_ARG;
    }

    device = frame->format == AV_PIX_FMT_VIDEOTOOLBOX;

    if ((device && frame->data[3] == NULL) ||
        (!device &&
         (frame->linesize[0] <= 0 || frame->linesize[1] < 0 || frame->linesize[2] < 0))) {
        return HDRG_ERR_INVALID_ARG;
    }

    /* the session reads its format's rows from every plane: the frame must have them */
    if (own.width != format->width || own.height != format->height ||
        own_input.layout != input->layout ||
        (input->layout != 0 &&
         (own_input.bits != input->bits || own_input.interleaved != input->interleaved ||
          own_input.msb != input->msb)) ||
        (input->layout == 0 &&
         (own.bit_depth != format->bit_depth || own.has_alpha != format->has_alpha ||
          own.stride != format->stride))) {
        return HDRG_ERR_INVALID_ARG;
    }

    held = (hdrg_ff_held_t *)av_mallocz(sizeof(*held));
    if (held == NULL) {
        return HDRG_ERR_NOMEM;
    }

    held->ref = av_frame_clone(frame);
    if (held->ref == NULL) {
        av_free(held);

        return HDRG_ERR_NOMEM;
    }

    memset(out, 0, sizeof(*out));

    if (device) {
        held->device.kind = HDRG_FRAME_CVPIXELBUFFER;
        held->device.handles[0] = held->ref->data[3];
        out->device = &held->device;
    }

    for (p = 0; !device && p < 3u; p++) {
        out->planes[p] = held->ref->data[p];
        out->strides[p] = (size_t)held->ref->linesize[p];
    }

    if (hdrg_ff_film(frame, held->film)) {
        out->film_grain = held->film;
        out->film_grain_size = sizeof(held->film);
    }

    out->pts = frame->pts;
    out->release = hdrg_ff_release;
    out->opaque = held;

    return HDRG_OK;
}

extern uint32_t hdrg_ff_device(const AVFrame *frame)
{
    return (frame != NULL && frame->format == AV_PIX_FMT_VIDEOTOOLBOX) ? HDRG_DEVICE_METAL : 0u;
}

extern enum AVPixelFormat hdrg_ff_output_format(const hdrg_session_pixfmt_t *output)
{
    if (output == NULL) {
        return AV_PIX_FMT_NONE;
    }

    if (output->layout == HDRG_YUV_420 && output->bits == 10) {
        return output->interleaved ? (output->msb ? AV_PIX_FMT_P010 : AV_PIX_FMT_NONE)
                                   : (output->msb ? AV_PIX_FMT_NONE : AV_PIX_FMT_YUV420P10);
    }

    if (output->layout == 0) {
        return (output->bits == 16) ? AV_PIX_FMT_RGB48
                                    : ((output->bits == 8) ? AV_PIX_FMT_RGB24 : AV_PIX_FMT_NONE);
    }

    return AV_PIX_FMT_NONE;
}

extern AVFrame *hdrg_ff_pool_frame(AVBufferPool **pool, int width, int height,
                                   enum AVPixelFormat format)
{
    AVFrame *frame;
    uint8_t *start, p;
    int lines[4];
    size_t offsets[4], bytes;

    if (pool == NULL || !hdrg_ff_layout(width, height, format, lines, offsets, &bytes)) {
        return NULL;
    }

    if (*pool == NULL) {
        *pool = av_buffer_pool_init(bytes, av_buffer_alloc);
        if (*pool == NULL) {
            return NULL;
        }
    }

    frame = av_frame_alloc();
    if (frame == NULL) {
        return NULL;
    }

    /* (a pool of smaller frames: refused) */
    frame->buf[0] = av_buffer_pool_get(*pool);
    if (frame->buf[0] == NULL || frame->buf[0]->size < bytes) {
        av_frame_free(&frame);

        return NULL;
    }

    start = (uint8_t *)FFALIGN((uintptr_t)frame->buf[0]->data, (uintptr_t)HDRG_FF_ALIGN);
    frame->format = format;
    frame->width = width;
    frame->height = height;

    for (p = 0; p < 4u; p++) {
        frame->linesize[p] = lines[p];
        frame->data[p] = (lines[p] > 0) ? start + offsets[p] : NULL;
    }

    frame->extended_data = frame->data;

    return frame;
}

extern AVFrame *hdrg_ff_output_frame(int width, int height, const hdrg_session_pixfmt_t *output,
                                     AVBufferPool **pool, hdrg_session_output_t *out)
{
    AVFrame *frame;
    uint8_t p;
    int32_t format;

    format = (int32_t)hdrg_ff_output_format(output);
    if (format == AV_PIX_FMT_NONE || out == NULL || width <= 0 || height <= 0) {
        return NULL;
    }

    if (pool != NULL) {
        frame = hdrg_ff_pool_frame(pool, width, height, (enum AVPixelFormat)format);
        if (frame == NULL) {
            return NULL;
        }
    } else {
        frame = av_frame_alloc();
        if (frame == NULL) {
            return NULL;
        }

        frame->format = format;
        frame->width = width;
        frame->height = height;

        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);

            return NULL;
        }
    }

    memset(out, 0, sizeof(*out));

    for (p = 0; p < 3u; p++) {
        out->planes[p] = frame->data[p];
        out->strides[p] = (size_t)frame->linesize[p];
    }

    return frame;
}

extern hdrg_status_t hdrg_ff_output_device(const AVFrame *frame, hdrg_device_frame_t *device,
                                           hdrg_session_output_t *out)
{
    if (frame == NULL || device == NULL || out == NULL ||
        frame->format != AV_PIX_FMT_VIDEOTOOLBOX || frame->data[3] == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    memset(device, 0, sizeof(*device));
    memset(out, 0, sizeof(*out));
    device->kind = HDRG_FRAME_CVPIXELBUFFER;
    device->handles[0] = frame->data[3];
    out->device = device;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_ff_finish(AVFrame *frame, const hdrg_session_output_t *out,
                                    float peak_nits, bool bt2020)
{
    const AVPixFmtDescriptor *desc;
    AVMasteringDisplayMetadata *master;
    AVDynamicHDRPlus *plus;
    hdrg_status_t status;
    uint8_t t35[HDRG_HDR10PLUS_T35_BYTES];
    size_t size;
    bool rgb;

    if (frame == NULL || out == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    desc = av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
    if (desc == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    rgb = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;

    frame->pts = out->pts;
    frame->color_trc = AVCOL_TRC_SMPTE2084;
    frame->color_primaries = (!rgb || bt2020) ? AVCOL_PRI_BT2020 : AVCOL_PRI_SMPTE432;
    frame->colorspace = rgb ? AVCOL_SPC_RGB : AVCOL_SPC_BT2020_NCL;
    frame->color_range = rgb ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    frame->chroma_location = rgb ? AVCHROMA_LOC_UNSPECIFIED : AVCHROMA_LOC_LEFT;

    av_frame_remove_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);

    master = av_mastering_display_metadata_create_side_data(frame);
    if (master == NULL) {
        return HDRG_ERR_NOMEM;
    }

    hdrg_ff_master(master, peak_nits);

    status = hdrg_ff_st2094_50(frame, &out->st2094_50);
    if (status != HDRG_OK) {
        return status;
    }

    av_frame_remove_side_data(frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);

    /* HDR10+: its T.35 (one window of the whole frame, the display's own tone mapping) */
    size = hdrg_hdr10plus_t35(&out->hdr10plus, t35, sizeof(t35));
    if (size == 0) {
        return HDRG_OK;
    }

    plus = av_dynamic_hdr_plus_create_side_data(frame);
    if (plus == NULL) {
        return HDRG_ERR_NOMEM;
    }

    if (av_dynamic_hdr_plus_from_t35(plus, t35, size) < 0) {
        return HDRG_ERR_INTERNAL;
    }

    plus->itu_t_t35_country_code = 0xB5;

    return HDRG_OK;
}

extern hdrg_status_t hdrg_ff_hdr_side_data(AVFrameSideData ***sd, int *nb_sd, float peak_nits,
                                           float max_cll, float max_fall)
{
    AVContentLightMetadata *light;
    AVFrameSideData *entry;

    if (sd == NULL || nb_sd == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    entry =
        av_frame_side_data_new(sd, nb_sd, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
                               sizeof(AVMasteringDisplayMetadata), AV_FRAME_SIDE_DATA_FLAG_UNIQUE);
    if (entry == NULL) {
        return HDRG_ERR_NOMEM;
    }

    hdrg_ff_master((AVMasteringDisplayMetadata *)(void *)entry->data, peak_nits);

    if (max_cll <= 0.0f) {
        return HDRG_OK;
    }

    entry = av_frame_side_data_new(sd, nb_sd, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL,
                                   sizeof(AVContentLightMetadata), AV_FRAME_SIDE_DATA_FLAG_UNIQUE);
    if (entry == NULL) {
        return HDRG_ERR_NOMEM;
    }

    light = (AVContentLightMetadata *)(void *)entry->data;
    light->MaxCLL = (unsigned)(max_cll + 0.5f);
    light->MaxFALL = (unsigned)((max_fall > 0.0f ? max_fall : 0.0f) + 0.5f);

    return HDRG_OK;
}

extern hdrg_status_t hdrg_ff_copy_props(AVFrame *dst, const AVFrame *src)
{
    size_t i;

    if (dst == NULL || src == NULL) {
        return HDRG_ERR_INVALID_ARG;
    }

    if (av_frame_copy_props(dst, src) < 0) {
        return HDRG_ERR_NOMEM;
    }

    for (i = 0; i < HDRG_FF_DROPPED; i++) {
        av_frame_remove_side_data(dst, hdrg_ff_dropped[i]);
    }

    return HDRG_OK;
}

extern bool hdrg_ff_keeps(enum AVFrameSideDataType type)
{
    size_t i;

    for (i = 0; i < HDRG_FF_DROPPED; i++) {
        if (hdrg_ff_dropped[i] == type) {
            return false;
        }
    }

    return true;
}
