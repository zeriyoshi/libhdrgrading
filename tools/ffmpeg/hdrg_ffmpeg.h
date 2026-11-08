/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading and FFmpeg frames: a session's format from
 * a frame's pixel format and colors, frames by reference, output frames with
 * their colors and HDR side data, and which side data an output keeps. It
 * depends on libavutil (FFmpeg 7.0 or later) and the public headers only;
 * the library itself does not depend on FFmpeg.
 */
#ifndef HDRG_FFMPEG_H
#define HDRG_FFMPEG_H

#include <stdbool.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#include "hdrgrading/filter.h"
#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The session's frame format and input pixels for frames like frame (its
 * size, pixel format and colors): Y'CbCr 4:2:0 / 4:2:2 / 4:4:4 of 8 to 16
 * bits in the host's byte order, Cb and Cr in a plane each or alternating
 * in one (nv12, p010: the bits in the samples' low or high bits; yuvj*:
 * full range), VideoToolbox's frames of those (their sw_format; a session
 * of config.device hdrg_ff_device(frame)), or packed rgb24 / rgba / rgb48 /
 * rgba64 (the alpha not read). Unspecified colors are BT.709 (the matrix
 * from the primaries), limited range Y'CbCr and full range R'G'B'; R'G'B'
 * takes the sRGB curve unless given. The chroma location is the frame's
 * (unspecified: LEFT, yuvj*: CENTER). Other pixel formats (nv21, planar
 * R'G'B', the other byte order) and HDR transfers (PQ, HLG) give
 * HDRG_ERR_UNSUPPORTED: convert those first (swscale)
 */
extern hdrg_status_t hdrg_ff_input(const AVFrame *frame, hdrg_session_format_t *format,
                                   hdrg_session_pixfmt_t *input);

/*
 * frame as a session frame of format and input (hdrg_ff_input's of the
 * session's first frame): a new reference to its buffers, dropped when the
 * session releases the pixels (no copy; a VideoToolbox frame's pixel buffer
 * as the frame's device, HDRG_FRAME_CVPIXELBUFFER). The frame must be of
 * the same size and samples (Y'CbCr layout, bits and interleaving; R'G'B'
 * bits, alpha and stride): HDRG_ERR_INVALID_ARG otherwise, as for a stream
 * whose size or pixel format changes mid-way (the session would read past
 * the frame's planes). The frame's AV1 film grain its decoder left out
 * (AV_FRAME_DATA_FILM_GRAIN_PARAMS: export_side_data film_grain on an AV1
 * decoder) goes with it as HDRG_SD_FILM_GRAIN, the session putting the grain
 * back; H.274's is not taken (leave such decoders to add their grain)
 */
extern hdrg_status_t hdrg_ff_frame(const AVFrame *frame, const hdrg_session_format_t *format,
                                   const hdrg_session_pixfmt_t *input, hdrg_session_frame_t *out);

/* the session's config.device a frame needs: HDRG_DEVICE_METAL for VideoToolbox's, else 0 */
extern uint32_t hdrg_ff_device(const AVFrame *frame);

/*
 * the pixel format of a session's output pixels: yuv420p10 (p010 when Cb and Cr alternate in the
 * high bits), rgb48, rgb24; NONE for others
 */
extern enum AVPixelFormat hdrg_ff_output_format(const hdrg_session_pixfmt_t *output);

/*
 * A VideoToolbox frame (an encoder's pool's: av_hwframe_get_buffer) as the
 * session's output on the GPU: its pixel buffer as device (the caller's,
 * read at the receive), out cleared to it. The session's output pixels must
 * be the frame's sw_format (p010: interleaved, msb)
 */
extern hdrg_status_t hdrg_ff_output_device(const AVFrame *frame, hdrg_device_frame_t *device,
                                           hdrg_session_output_t *out);

/*
 * A width x height frame of format, its buffer from *pool (made at the
 * first call for frames of that size and format; NULL on failure, as for a
 * pool of smaller frames): back in the pool when the frame and every
 * reference to it are freed, so frames in flight (the encoder's, the
 * session's) are not overwritten and the rest are used again. The caller
 * frees the pool with av_buffer_pool_uninit (it goes once its buffers are
 * back). Its rows and planes are aligned and padded as av_frame_get_buffer's
 */
extern AVFrame *hdrg_ff_pool_frame(AVBufferPool **pool, int width, int height,
                                   enum AVPixelFormat format);

/*
 * a width x height frame for the session's output pixels, its planes in out; its buffer from
 * *pool (hdrg_ff_pool_frame), or one of its own when pool is NULL; NULL on failure
 */
extern AVFrame *hdrg_ff_output_frame(int width, int height, const hdrg_session_pixfmt_t *output,
                                     AVBufferPool **pool, hdrg_session_output_t *out);

/*
 * The received output's pts, colors (PQ; Y'CbCr: the BT.2020 matrix and
 * container, limited range, chroma at the left; R'G'B': full range,
 * BT.2020 when bt2020, else Display P3) and mastering display side data
 * (Display P3 D65, 0.005 cd/m^2 to peak_nits: the stream's,
 * hdrg_session_peak) on frame, and the frame's HDR10+ (one window, no tone
 * mapping curve) and ST 2094-50 (FFmpeg 9 and later: the gain curve back to
 * the SDR for headroom 0) when the session measured them
 */
extern hdrg_status_t hdrg_ff_finish(AVFrame *frame, const hdrg_session_output_t *out,
                                    float peak_nits, bool bt2020);

/*
 * The mastering display up to peak_nits (hdrg_session_peak) and, when
 * max_cll > 0, the content light level appended to an array of side data
 * (an encoder's AVCodecContext.decoded_side_data)
 */
extern hdrg_status_t hdrg_ff_hdr_side_data(AVFrameSideData ***sd, int *nb_sd, float peak_nits,
                                           float max_cll, float max_fall);

/*
 * The properties of an input frame onto its output (av_frame_copy_props)
 * without the side data that describes the SDR signal or its coding:
 * mastering display, content light level, HDR10+, HDR Vivid, Dolby Vision,
 * ICC profile, ambient viewing environment, film grain parameters, the
 * encoder's parameters. Timecodes, captions, SEI and the rest are kept
 */
extern hdrg_status_t hdrg_ff_copy_props(AVFrame *dst, const AVFrame *src);

/* whether hdrg_ff_copy_props keeps side data of type */
extern bool hdrg_ff_keeps(enum AVFrameSideDataType type);

#ifdef __cplusplus
}
#endif

#endif /* HDRG_FFMPEG_H */
