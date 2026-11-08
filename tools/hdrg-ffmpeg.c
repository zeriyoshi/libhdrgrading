/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * hdrg-ffmpeg: an SDR video file into HDR through FFmpeg's libraries (linked
 * dynamically) and tools/ffmpeg: demux, decode, the
 * pixels into a format the session takes (swscale, without scaling), the
 * session, encode as 4:2:0 10-bit PQ of the BT.2020 container with the
 * mastering display, mux. Audio is copied. The session's settings are those
 * of hdrg-video, and any hdrg-cli setting by --set name=value. On Apple,
 * --hwaccel videotoolbox decodes on VideoToolbox and a *_videotoolbox
 * encoder takes P010 pixel buffers: their pixels stay on the GPU into and
 * out of a session that keeps its frames there (tools/ffmpeg's device
 * frames), else the CPU reads and writes them in place (av_hwframe_map).
 * The output frames and the swscale ones take their buffers from pools
 * (hdrg_ff_pool_frame), the next output's frame (a pixel buffer: mapped
 * once) kept until the session has a frame for it.
 * An AV1 decoder leaves its film grain out and gives its parameters, which
 * the session puts back as SDR showed the grain (--decoder-grain: the
 * decoder adds it).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include "common.h"
#include "hdrg_ffmpeg.h"

#define HFF_RING  (HDRG_SESSION_MAX_WINDOW + 4u) /* the sent frames' properties, in order */
#define HFF_FINE  90000 /* --out-rate: the session's pts in 1/90000 s (the outputs' exact) */
#define HFF_QUEUE 4096u /* packets of the other streams before the header */
#define HFF_META  256u  /* the encoded frames' dynamic metadata kept for their packets ... */
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 30, 100)
#define HFF_META_KINDS 2u /* ... HDR10+ and ST 2094-50 (FFmpeg 9) */
#else
#define HFF_META_KINDS 1u /* ... HDR10+ */
#endif

/* the dynamic metadata's frame and packet side data */
static const enum AVFrameSideDataType hff_meta_frame[2] = {
    AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 30, 100)
    AV_FRAME_DATA_DYNAMIC_HDR_SMPTE_2094_APP5,
#else
    AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
#endif
};
static const enum AVPacketSideDataType hff_meta_packet[2] = {
    AV_PKT_DATA_DYNAMIC_HDR10_PLUS,
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(62, 30, 100)
    AV_PKT_DATA_DYNAMIC_HDR_SMPTE_2094_APP5,
#else
    AV_PKT_DATA_DYNAMIC_HDR10_PLUS,
#endif
};

typedef struct {
    AVFormatContext *in, *out;
    AVCodecContext *dec, *enc;
    AVBufferRef *hw_device, *hw_frames; /* VideoToolbox's device, the encoder's P010 pool */
    AVBufferPool *out_pool, *sws_pool;  /* the output frames' and the converted frames' buffers */
    struct SwsContext *sws;
    AVFrame *next, *next_map; /* the next output's frame (a pixel buffer: mapped), kept while the
                                 session has no frame for it */
    AVFrame *props[HFF_RING];
    AVPacket *queued[HFF_QUEUE];
    AVBufferRef *meta[HFF_META][HFF_META_KINDS]; /* the frames' dynamic metadata (their side
                                                    data's buffers) by meta_pts */
    AVRational tb;       /* the session's pts': the stream's, 1/HFF_FINE with --out-rate */
    AVRational out_rate; /* --out-rate (0/1: the input's) */
    AVDictionary *enc_opts;
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_device_frame_t next_device; /* next's pixel buffer on the GPU */
    const char *encoder;
    int32_t *map;                /* the input streams' output streams, -1 = dropped */
    int64_t props_pts[HFF_RING]; /* the kept properties' frames' pts (the session's) */
    int64_t meta_pts[HFF_META];  /* ... the encoder's */
    uint64_t frames, max_frames;
    uint32_t props_head, props_count, queue_count, threads, meta_next;
    uint8_t gpu;                      /* --gpu: HDRG_GPU_* */
    uint8_t gpu_analysis;             /* --gpu-analysis: HDRG_GPU_ANALYSIS_* */
    int32_t video, out_video, target; /* target: the converted pixel format, NONE = as decoded */
    int32_t first_width, first_height, first_format; /* the decoded frames' (the first one's) */
    bool audio, header;
    bool hwaccel;       /* --hwaccel videotoolbox */
    bool decoder_grain; /* --decoder-grain: AV1's decoder adds its film grain */
    bool vt_out;        /* a VideoToolbox encoder: P010 pixel buffers of the pool */
    bool on_device;     /* the session takes and gives the pixel buffers on the GPU */
    float given_cll, given_fall, peak, max_cll, max_fall; /* peak: the mastering display's */
} hff_t;

static void usage(void)
{
    fprintf(stderr,
            "usage: hdrg-ffmpeg [options] <input> <output>\n"
            "\n"
            "An SDR video file into HDR: the first video stream graded into 4:2:0 10-bit\n"
            "SMPTE ST 2084 (PQ) of the BT.2020 container and matrix (limited range), with\n"
            "the mastering display (Display P3 D65 up to the rendition peak) in the\n"
            "stream; audio streams are copied. The output format follows the file name.\n"
            "\n"
            "options:\n"
            "  --encoder <name>      FFmpeg video encoder (default libsvtav1, else libx265)\n"
            "  --enc-opt <k=v>       an encoder option (repeatable), e.g. crf=30, preset=6\n"
            "  --max-cll <c,f>       content light level to declare (MaxCLL, MaxFALL); the\n"
            "                        measured values are printed at the end\n"
            "  --no-audio            drop the audio streams\n"
            "  --preset <name>       photo (default), anime or game\n"
            "  --set <name=value>    any hdrg-cli setting (diffuse-max=1.5, peak-nits=1000, ...)\n"
            "  --temporal <mode>     windowed (default), recursive or static\n"
            "  --past <n> / --future <n> / --proxy <n>   as hdrg-video\n"
            "  --no-flash-limit      no flash limiter\n"
            "  --no-dither           no dither of the output\n"
            "  --threads <n>         the library's threads (0 = auto)\n"
            "  --gpu <mode>          off, auto or filters: as hdrg-video\n"
            "  --gpu-analysis <a>    exact (default) or fast: as hdrg-video\n"
            "  --latency <profile>   offline (default), playback or low: as hdrg-video\n"
            "  --out-rate <num:den>  the output's frame rate: the frames between the input's\n"
            "                        interpolated (the input's rate needed; default its own)\n"
            "  --hwaccel <name>      videotoolbox: decode on the GPU (Apple; --gpu auto)\n"
            "  --decoder-grain       an AV1 decoder adds its film grain (default: its\n"
            "                        parameters to the session, the grain put back in PQ)\n"
            "  --frames <n>          stop after n frames\n");
}

static void hff_error(const char *what, int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(code, text, sizeof(text));
    fprintf(stderr, "hdrg-ffmpeg: %s: %s\n", what, text);
}

static bool hff_status(const char *what, hdrg_status_t status)
{
    if (status == HDRG_OK) {
        return true;
    }

    fprintf(stderr, "hdrg-ffmpeg: %s: %s\n", what, hdrg_status_string(status));

    return false;
}

/* a planar pixel format the session takes for frames of format: NONE when it takes them */
static int32_t hff_target(const AVFrame *frame)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
    hdrg_session_pixfmt_t input;
    hdrg_session_format_t format;
    bool deep;

    if (hdrg_ff_input(frame, &format, &input) != HDRG_ERR_UNSUPPORTED || desc == NULL) {
        return AV_PIX_FMT_NONE;
    }

    deep = desc->comp[0].depth > 8;

    if ((desc->flags & AV_PIX_FMT_FLAG_RGB) != 0) {
        return deep ? AV_PIX_FMT_RGB48 : AV_PIX_FMT_RGB24;
    }

    if (desc->nb_components >= 3 && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1) {
        return deep ? AV_PIX_FMT_YUV420P16 : AV_PIX_FMT_YUV420P;
    }

    if (desc->nb_components >= 3 && desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0) {
        return deep ? AV_PIX_FMT_YUV422P16 : AV_PIX_FMT_YUV422P;
    }

    return deep ? AV_PIX_FMT_YUV444P16 : AV_PIX_FMT_YUV444P;
}

/* the decoder's VideoToolbox frames where it offers them */
static enum AVPixelFormat hff_get_format(AVCodecContext *dec, const enum AVPixelFormat *formats)
{
    const enum AVPixelFormat *f;

    (void)dec;

    for (f = formats; *f != AV_PIX_FMT_NONE; f++) {
        if (*f == AV_PIX_FMT_VIDEOTOOLBOX) {
            return *f;
        }
    }

    return formats[0];
}

/* VideoToolbox's device, once */
static bool hff_hw_device(hff_t *s)
{
    int code;

    if (s->hw_device != NULL) {
        return true;
    }

    code = av_hwdevice_ctx_create(&s->hw_device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, NULL, NULL, 0);
    if (code < 0) {
        hff_error("VideoToolbox", code);

        return false;
    }

    return true;
}

static bool hff_open_input(hff_t *s, const char *path)
{
    const AVCodec *codec;
    AVStream *st;
    int code;

    code = avformat_open_input(&s->in, path, NULL, NULL);
    if (code >= 0) {
        code = avformat_find_stream_info(s->in, NULL);
    }

    if (code < 0) {
        hff_error(path, code);

        return false;
    }

    s->video = av_find_best_stream(s->in, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (s->video < 0) {
        hff_error("no video stream", s->video);

        return false;
    }

    st = s->in->streams[s->video];
    s->dec = avcodec_alloc_context3(codec);
    if (s->dec == NULL || avcodec_parameters_to_context(s->dec, st->codecpar) < 0) {
        fprintf(stderr, "hdrg-ffmpeg: the decoder could not be set up\n");

        return false;
    }

    s->dec->pkt_timebase = st->time_base;

    /* AV1's film grain: its parameters to the session in place of the decoder's grain */
    if (codec->id == AV_CODEC_ID_AV1 && !s->decoder_grain) {
        s->dec->export_side_data |= AV_CODEC_EXPORT_DATA_FILM_GRAIN;
    }

    if (s->hwaccel) {
        if (!hff_hw_device(s)) {
            return false;
        }

        s->dec->hw_device_ctx = av_buffer_ref(s->hw_device);
        s->dec->get_format = hff_get_format;
    }

    code = avcodec_open2(s->dec, codec, NULL);
    if (code < 0) {
        hff_error("decoder", code);

        return false;
    }

    return true;
}

/* the output and its streams: the video's parameters come when the encoder opens */
static bool hff_open_output(hff_t *s, const char *path)
{
    AVStream *in, *out;
    uint32_t i;
    int code;

    code = avformat_alloc_output_context2(&s->out, NULL, NULL, path);
    if (code < 0) {
        hff_error(path, code);

        return false;
    }

    s->map = (int32_t *)calloc(s->in->nb_streams, sizeof(*s->map));
    if (s->map == NULL) {
        return false;
    }

    for (i = 0; i < s->in->nb_streams; i++) {
        in = s->in->streams[i];
        s->map[i] = -1;

        if ((int32_t)i != s->video &&
            (!s->audio || in->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)) {
            continue;
        }

        out = avformat_new_stream(s->out, NULL);
        if (out == NULL) {
            return false;
        }

        s->map[i] = out->index;

        if ((int32_t)i == s->video) {
            s->out_video = out->index;
            out->time_base = in->time_base;

            continue;
        }

        if (avcodec_parameters_copy(out->codecpar, in->codecpar) < 0) {
            return false;
        }

        out->codecpar->codec_tag = 0;
        out->time_base = in->time_base;
    }

    if ((s->out->oformat->flags & AVFMT_NOFILE) == 0) {
        code = avio_open(&s->out->pb, path, AVIO_FLAG_WRITE);
        if (code < 0) {
            hff_error(path, code);

            return false;
        }
    }

    return true;
}

/* the session for frames like frame, through swscale when it does not take their format */
static bool hff_start(hff_t *s, const AVFrame *frame)
{
    AVFrame *probe;
    hdrg_status_t status;

    s->target = hff_target(frame);
    s->first_width = frame->width;
    s->first_height = frame->height;
    s->first_format = frame->format;
    probe = NULL;

    /* the converted frames' pool's (the stride the session takes for R'G'B') */
    if (s->target != AV_PIX_FMT_NONE) {
        s->sws = sws_getContext(frame->width, frame->height, (enum AVPixelFormat)frame->format,
                                frame->width, frame->height, (enum AVPixelFormat)s->target,
                                SWS_POINT, NULL, NULL, NULL);
        probe = hdrg_ff_pool_frame(&s->sws_pool, frame->width, frame->height,
                                   (enum AVPixelFormat)s->target);
        if (s->sws == NULL || probe == NULL || av_frame_copy_props(probe, frame) < 0) {
            av_frame_free(&probe);
            fprintf(stderr, "hdrg-ffmpeg: no conversion from %s\n",
                    av_get_pix_fmt_name((enum AVPixelFormat)frame->format));

            return false;
        }
    }

    status = hdrg_ff_input((probe != NULL) ? probe : frame, &s->format, &s->config.input);
    av_frame_free(&probe);

    if (!hff_status("the input", status)) {
        return false;
    }

    s->config.output.layout = HDRG_YUV_420;
    s->config.output.matrix = HDRG_MATRIX_BT2020;
    s->config.output.bits = 10;
    s->config.output.full_range = false;
    s->config.output.interleaved = s->vt_out;
    s->config.output.msb = s->vt_out;
    s->caps.gamut = HDRG_GAMUT_BT2020;

    if (s->in->streams[s->video]->avg_frame_rate.num > 0) {
        s->config.rate_num = (uint32_t)s->in->streams[s->video]->avg_frame_rate.num;
        s->config.rate_den = (uint32_t)s->in->streams[s->video]->avg_frame_rate.den;
    }

    s->tb = (s->out_rate.num > 0) ? (AVRational){1, HFF_FINE} : s->in->streams[s->video]->time_base;

    /* another output rate: the input's rate, pts fine enough for the frames between */
    if (s->out_rate.num > 0) {
        if (s->config.rate_num == 0) {
            fprintf(stderr, "hdrg-ffmpeg: --out-rate needs the input's frame rate\n");

            return false;
        }

        s->config.out_rate_num = (uint32_t)s->out_rate.num;
        s->config.out_rate_den = (uint32_t)s->out_rate.den;
    }

    /* pixel buffers on the GPU when the session keeps its frames there, else mapped in place */
    status = HDRG_ERR_UNSUPPORTED;

    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX || s->vt_out) {
        s->config.device = HDRG_DEVICE_METAL;
        status =
            hdrg_session_create(s->ctx, &s->format, &s->params, &s->caps, &s->config, &s->session);
        s->on_device = status == HDRG_OK;
        s->config.device = 0;
    }

    if (status == HDRG_ERR_UNSUPPORTED) {
        status =
            hdrg_session_create(s->ctx, &s->format, &s->params, &s->caps, &s->config, &s->session);
    }

    if (status == HDRG_OK) {
        s->peak = hdrg_session_peak(s->session);
    }

    if (s->hwaccel || s->vt_out) {
        fprintf(stderr, "hdrg-ffmpeg: %s frames %s\n",
                (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) ? "VideoToolbox" : "decoded",
                s->on_device ? "on the GPU" : "in host memory");
    }

    return hff_status("the session", status);
}

/* the encoder's pool of P010 pixel buffers (a VideoToolbox encoder) */
static bool hff_hw_frames(hff_t *s)
{
    AVHWFramesContext *frames;
    int code;

    if (!hff_hw_device(s)) {
        return false;
    }

    s->hw_frames = av_hwframe_ctx_alloc(s->hw_device);
    if (s->hw_frames == NULL) {
        return false;
    }

    frames = (AVHWFramesContext *)(void *)s->hw_frames->data;
    frames->format = AV_PIX_FMT_VIDEOTOOLBOX;
    frames->sw_format = AV_PIX_FMT_P010;
    frames->width = (int)s->format.width;
    frames->height = (int)s->format.height;
    code = av_hwframe_ctx_init(s->hw_frames);
    if (code < 0) {
        hff_error("the pixel buffer pool", code);

        return false;
    }

    return true;
}

static bool hff_write(hff_t *s, AVPacket *packet)
{
    int code;

    code = av_interleaved_write_frame(s->out, packet);
    if (code < 0) {
        hff_error("mux", code);

        return false;
    }

    return true;
}

/*
 * the dynamic metadata of the encoder's frame of pts onto its packet where the encoder did not
 * put it (Matroska writes them as BlockAdditionals; libaom and libvpx carry them in the stream)
 */
static bool hff_packet_meta(hff_t *s, AVPacket *packet)
{
    uint32_t i, k;
    uint8_t *data;

    for (i = 0; i < HFF_META; i++) {
        for (k = 0; s->meta_pts[i] == packet->pts && k < HFF_META_KINDS; k++) {
            if (s->meta[i][k] == NULL ||
                av_packet_get_side_data(packet, hff_meta_packet[k], NULL) != NULL) {
                continue;
            }

            data = av_packet_new_side_data(packet, hff_meta_packet[k], s->meta[i][k]->size);
            if (data == NULL) {
                return false;
            }

            memcpy(data, s->meta[i][k]->data, s->meta[i][k]->size);
        }
    }

    return true;
}

/* a packet of another input stream into its output stream (the time base the muxer settled) */
static bool hff_forward(hff_t *s, AVPacket *packet)
{
    const AVStream *in = s->in->streams[packet->stream_index];

    packet->stream_index = s->map[packet->stream_index];
    av_packet_rescale_ts(packet, in->time_base, s->out->streams[packet->stream_index]->time_base);

    return hff_write(s, packet);
}

/* the encoder, at the first output: its mastering display needs the rendition peak */
static bool hff_open_encoder(hff_t *s, float peak)
{
    const AVCodec *codec;
    AVDictionary *opts;
    AVStream *st, *in;
    AVPacketSideData *entry;
    AVMasteringDisplayMetadata *master;
    AVContentLightMetadata *light;
    uint32_t i;
    int code;

    codec = (s->encoder != NULL) ? avcodec_find_encoder_by_name(s->encoder)
                                 : avcodec_find_encoder_by_name("libsvtav1");
    codec = (codec == NULL && s->encoder == NULL) ? avcodec_find_encoder_by_name("libx265") : codec;
    if (codec == NULL) {
        fprintf(stderr, "hdrg-ffmpeg: no encoder %s\n",
                (s->encoder != NULL) ? s->encoder : "libsvtav1 / libx265");

        return false;
    }

    in = s->in->streams[s->video];
    st = s->out->streams[s->out_video];
    s->enc = avcodec_alloc_context3(codec);
    if (s->enc == NULL) {
        return false;
    }

    s->enc->width = (int)s->format.width;
    s->enc->height = (int)s->format.height;
    s->enc->pix_fmt = s->vt_out ? AV_PIX_FMT_VIDEOTOOLBOX : AV_PIX_FMT_YUV420P10;

    if (s->vt_out) {
        s->enc->hw_frames_ctx = av_buffer_ref(s->hw_frames);
        if (s->enc->hw_frames_ctx == NULL) {
            return false;
        }
    }

    s->enc->time_base = (s->out_rate.num > 0) ? av_inv_q(s->out_rate) : in->time_base;
    s->enc->framerate = (s->out_rate.num > 0) ? s->out_rate : av_guess_frame_rate(s->in, in, NULL);
    s->enc->sample_aspect_ratio = s->dec->sample_aspect_ratio;
    s->enc->color_primaries = AVCOL_PRI_BT2020;
    s->enc->color_trc = AVCOL_TRC_SMPTE2084;
    s->enc->colorspace = AVCOL_SPC_BT2020_NCL;
    s->enc->color_range = AVCOL_RANGE_MPEG;
    s->enc->chroma_sample_location = AVCHROMA_LOC_LEFT;

    if ((s->out->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        s->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (!hff_status("the HDR side data",
                    hdrg_ff_hdr_side_data(&s->enc->decoded_side_data, &s->enc->nb_decoded_side_data,
                                          peak, s->given_cll, s->given_fall))) {
        return false;
    }

    opts = NULL;
    av_dict_copy(&opts, s->enc_opts, 0);
    code = avcodec_open2(s->enc, codec, &opts);
    av_dict_free(&opts);

    if (code < 0) {
        hff_error("encoder", code);

        return false;
    }

    if (avcodec_parameters_from_context(st->codecpar, s->enc) < 0) {
        return false;
    }

    /* HEVC in MP4 / QuickTime as hvc1: Apple's players do not take hev1 */
    if (st->codecpar->codec_id == AV_CODEC_ID_HEVC &&
        av_match_name(s->out->oformat->name, "mp4,mov")) {
        st->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    }

    st->time_base = s->enc->time_base;
    st->avg_frame_rate = s->enc->framerate;

    /* the stream's own HDR metadata, for the containers that carry it (Matroska, MP4) */
    for (i = 0; i < (uint32_t)s->enc->nb_decoded_side_data; i++) {
        if (s->enc->decoded_side_data[i]->type != AV_FRAME_DATA_MASTERING_DISPLAY_METADATA &&
            s->enc->decoded_side_data[i]->type != AV_FRAME_DATA_CONTENT_LIGHT_LEVEL) {
            continue;
        }

        if (s->enc->decoded_side_data[i]->type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA) {
            entry = av_packet_side_data_new(
                &st->codecpar->coded_side_data, &st->codecpar->nb_coded_side_data,
                AV_PKT_DATA_MASTERING_DISPLAY_METADATA, sizeof(*master), 0);
        } else {
            entry = av_packet_side_data_new(&st->codecpar->coded_side_data,
                                            &st->codecpar->nb_coded_side_data,
                                            AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sizeof(*light), 0);
        }

        if (entry == NULL) {
            return false;
        }

        memcpy(entry->data, s->enc->decoded_side_data[i]->data, entry->size);
    }

    code = avformat_write_header(s->out, NULL);
    if (code < 0) {
        hff_error("header", code);

        return false;
    }

    s->header = true;

    for (i = 0; i < s->queue_count; i++) {
        if (!hff_forward(s, s->queued[i])) {
            return false;
        }

        av_packet_free(&s->queued[i]);
    }

    s->queue_count = 0;

    return true;
}

/* the encoder's packets for frame (NULL: the end) */
static bool hff_encode(hff_t *s, const AVFrame *frame)
{
    AVPacket *packet;
    int code;

    code = avcodec_send_frame(s->enc, frame);
    if (code < 0) {
        hff_error("encode", code);

        return false;
    }

    packet = av_packet_alloc();
    if (packet == NULL) {
        return false;
    }

    for (;;) {
        code = avcodec_receive_packet(s->enc, packet);
        if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) {
            break;
        }

        if (code < 0) {
            hff_error("encode", code);
            av_packet_free(&packet);

            return false;
        }

        if (!hff_packet_meta(s, packet)) {
            av_packet_free(&packet);

            return false;
        }

        av_packet_rescale_ts(packet, s->enc->time_base, s->out->streams[s->out_video]->time_base);
        packet->stream_index = s->out_video;

        if (!hff_write(s, packet)) {
            av_packet_free(&packet);

            return false;
        }
    }

    av_packet_free(&packet);

    return true;
}

/*
 * a frame for the next output: host memory of the pool, or a pixel buffer of the encoder's pool
 * on the GPU or mapped (next_map) for the CPU
 */
static bool hff_next(hff_t *s)
{
    hdrg_session_output_t planes;

    if (!s->vt_out) {
        s->next = hdrg_ff_output_frame((int)s->format.width, (int)s->format.height,
                                       &s->config.output, &s->out_pool, &planes);

        return s->next != NULL;
    }

    if ((s->hw_frames == NULL && !hff_hw_frames(s)) || (s->next = av_frame_alloc()) == NULL) {
        return false;
    }

    if (av_hwframe_get_buffer(s->hw_frames, s->next, 0) < 0) {
        av_frame_free(&s->next);

        return false;
    }

    if (s->on_device) {
        return true;
    }

    s->next_map = av_frame_alloc();
    if (s->next_map == NULL ||
        av_hwframe_map(s->next_map, s->next, AV_HWFRAME_MAP_WRITE | AV_HWFRAME_MAP_OVERWRITE) < 0) {
        av_frame_free(&s->next_map);
        av_frame_free(&s->next);

        return false;
    }

    return true;
}

/*
 * the session's output into the next output's frame (made once and kept while the session has
 * no frame for it): its planes, or its pixel buffer on the GPU
 */
static bool hff_output(hff_t *s, hdrg_session_output_t *out)
{
    const AVFrame *pixels;
    uint8_t p;

    if (s->next == NULL && !hff_next(s)) {
        return false;
    }

    if (s->vt_out && s->on_device) {
        return hdrg_ff_output_device(s->next, &s->next_device, out) == HDRG_OK;
    }

    pixels = s->vt_out ? s->next_map : s->next;
    memset(out, 0, sizeof(*out));

    for (p = 0; p < 3u; p++) {
        out->planes[p] = pixels->data[p];
        out->strides[p] = (size_t)pixels->linesize[p];
    }

    return true;
}

/* the session's outputs into the encoder until it waits for frames (or ends) */
static bool hff_drain(hff_t *s)
{
    const AVFrameSideData *meta;
    hdrg_session_output_t out;
    hdrg_status_t status;
    AVFrame *dst, *props;
    uint32_t k;
    int64_t want;
    bool ok;

    for (;;) {
        if (!hff_output(s, &out)) {
            return false;
        }

        /* none yet: the frame (and its mapping) kept for the next receive */
        status = hdrg_session_receive_frame(s->session, &out);
        if (status == HDRG_ERR_AGAIN) {
            return true;
        }

        dst = s->next;
        s->next = NULL;
        av_frame_free(&s->next_map);

        if (status == HDRG_EOF) {
            av_frame_free(&dst);

            return true;
        }

        if (!hff_status("the session", status)) {
            av_frame_free(&dst);

            return false;
        }

        /*
         * the source frame's properties without its SDR descriptions (a frame between: the
         * nearer one's; the frames before it no longer wanted); no forced key frames
         */
        want = !out.synthetic ? out.pts : out.src_pts[(out.phase < 32768u) ? 0 : 1];

        while (s->props_count > 1u && s->props_pts[s->props_head] < want) {
            av_frame_free(&s->props[s->props_head]);
            s->props_head = (s->props_head + 1u) % HFF_RING;
            s->props_count--;
        }

        props = (s->props_count > 0u) ? s->props[s->props_head] : NULL;
        ok = props == NULL || hdrg_ff_copy_props(dst, props) == HDRG_OK;

        /* without --out-rate one output a frame, in order (timestamps or none): its props done */
        if (s->out_rate.num == 0 && s->props_count > 0u) {
            av_frame_free(&s->props[s->props_head]);
            s->props_head = (s->props_head + 1u) % HFF_RING;
            s->props_count--;
        }

        dst->pict_type = AV_PICTURE_TYPE_NONE;
        dst->flags &= ~AV_FRAME_FLAG_KEY;
        ok = ok && hdrg_ff_finish(dst, &out, s->peak, true) == HDRG_OK;
        s->max_cll = (out.max_nits > s->max_cll) ? out.max_nits : s->max_cll;
        s->max_fall = (out.avg_nits > s->max_fall) ? out.avg_nits : s->max_fall;

        ok = ok && (s->enc != NULL || hff_open_encoder(s, s->peak));

        /* --out-rate: the output's frame number in the encoder's 1 / out_rate */
        if (ok && s->out_rate.num > 0) {
            dst->pts = av_rescale_q(out.pts, s->tb, s->enc->time_base);
            dst->duration = 1;
        }

        /* (its dynamic metadata kept for its packet) */
        for (k = 0; ok && k < HFF_META_KINDS; k++) {
            meta = av_frame_get_side_data(dst, hff_meta_frame[k]);
            av_buffer_unref(&s->meta[s->meta_next][k]);
            s->meta[s->meta_next][k] = (meta != NULL) ? av_buffer_ref(meta->buf) : NULL;
        }

        s->meta_pts[s->meta_next] = ok ? dst->pts : AV_NOPTS_VALUE;
        s->meta_next = (s->meta_next + 1u) % HFF_META;

        ok = ok && hff_encode(s, dst);
        av_frame_free(&dst);

        if (!ok) {
            return false;
        }
    }
}

/* a decoded frame into the session, its properties kept for its output */
static bool hff_frame(hff_t *s, const AVFrame *frame)
{
    const AVFrameSideData *film;
    const AVFrame *src;
    hdrg_session_frame_t in;
    hdrg_status_t status;
    AVFrameSideData *kept;
    AVFrame *converted, *props, *mapped;

    if (s->session == NULL && !hff_start(s, frame)) {
        return false;
    }

    converted = NULL;
    src = frame;

    /* the session (and the swscale context) were made for the first frame's size and format */
    if (frame->width != s->first_width || frame->height != s->first_height ||
        frame->format != s->first_format) {
        fprintf(stderr,
                "the stream changes its size or pixel format (%dx%d %s -> %dx%d %s): "
                "not supported\n",
                s->first_width, s->first_height,
                av_get_pix_fmt_name((enum AVPixelFormat)s->first_format), frame->width,
                frame->height, av_get_pix_fmt_name((enum AVPixelFormat)frame->format));

        return false;
    }

    /* (the pool's buffers: the session holds R'G'B' until it outputs it) */
    if (s->sws != NULL) {
        converted = hdrg_ff_pool_frame(&s->sws_pool, frame->width, frame->height,
                                       (enum AVPixelFormat)s->target);
        if (converted == NULL || av_frame_copy_props(converted, frame) < 0 ||
            sws_scale_frame(s->sws, converted, frame) < 0) {
            av_frame_free(&converted);

            return false;
        }

        src = converted;
    }

    /* a pixel buffer the session does not take on the GPU: read in place */
    mapped = NULL;

    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX && !s->on_device) {
        mapped = av_frame_alloc();
        if (mapped == NULL || av_hwframe_map(mapped, frame, AV_HWFRAME_MAP_READ) < 0) {
            av_frame_free(&mapped);
            fprintf(stderr, "hdrg-ffmpeg: a pixel buffer could not be mapped\n");

            return false;
        }

        mapped->color_primaries = frame->color_primaries;
        mapped->color_trc = frame->color_trc;
        mapped->colorspace = frame->colorspace;
        mapped->color_range = frame->color_range;
        mapped->chroma_location = frame->chroma_location;
        mapped->pts = frame->pts;
        src = mapped;
        film = av_frame_get_side_data(frame, AV_FRAME_DATA_FILM_GRAIN_PARAMS);
        kept = (film != NULL)
                   ? av_frame_new_side_data(mapped, AV_FRAME_DATA_FILM_GRAIN_PARAMS, film->size)
                   : NULL;

        if (kept != NULL) {
            memcpy(kept->data, film->data, film->size);
        }
    }

    status = hdrg_ff_frame(src, &s->format, &s->config.input, &in);
    av_frame_free(&converted);
    av_frame_free(&mapped);

    if (!hff_status("a frame", status)) {
        return false;
    }

    in.pts = frame->best_effort_timestamp;

    /* --out-rate: in 1/HFF_FINE (without a timestamp: the frame's place at the input's rate) */
    if (s->out_rate.num > 0) {
        in.pts = (in.pts != AV_NOPTS_VALUE)
                     ? av_rescale_q(in.pts, s->in->streams[s->video]->time_base, s->tb)
                     : av_rescale_q((int64_t)s->frames,
                                    (AVRational){(int)s->config.rate_den, (int)s->config.rate_num},
                                    s->tb);
    }

    props = av_frame_alloc();
    if (props == NULL || av_frame_copy_props(props, frame) < 0 || s->props_count == HFF_RING) {
        av_frame_free(&props);
        in.release(in.opaque);
        fprintf(stderr, "hdrg-ffmpeg: a frame's properties could not be kept\n");

        return false;
    }

    s->props[(s->props_head + s->props_count) % HFF_RING] = props;
    s->props_pts[(s->props_head + s->props_count) % HFF_RING] = in.pts;
    s->props_count++;

    for (;;) {
        status = hdrg_session_send_frame(s->session, &in);
        if (status != HDRG_ERR_AGAIN) {
            break;
        }

        if (!hff_drain(s)) {
            in.release(in.opaque);

            return false;
        }
    }

    if (status != HDRG_OK) {
        in.release(in.opaque);

        return hff_status("the session", status);
    }

    s->frames++;

    return hff_drain(s);
}

/* the decoder's frames for packet (NULL: the end) */
static bool hff_decode(hff_t *s, const AVPacket *packet)
{
    AVFrame *frame;
    int code;

    code = avcodec_send_packet(s->dec, packet);
    if (code < 0 && code != AVERROR_EOF) {
        hff_error("decode", code);

        return false;
    }

    frame = av_frame_alloc();
    if (frame == NULL) {
        return false;
    }

    for (;;) {
        code = avcodec_receive_frame(s->dec, frame);
        if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) {
            break;
        }

        if (code < 0) {
            hff_error("decode", code);
            av_frame_free(&frame);

            return false;
        }

        if (s->frames < s->max_frames && !hff_frame(s, frame)) {
            av_frame_free(&frame);

            return false;
        }

        av_frame_unref(frame);
    }

    av_frame_free(&frame);

    return true;
}

/* a packet of another stream: copied, or kept as read until the header is written */
static bool hff_copy(hff_t *s, AVPacket *packet)
{
    if (s->header) {
        return hff_forward(s, packet);
    }

    if (s->queue_count == HFF_QUEUE) {
        fprintf(stderr, "hdrg-ffmpeg: too many packets before the first video frame\n");

        return false;
    }

    s->queued[s->queue_count] = av_packet_clone(packet);

    return s->queued[s->queue_count++] != NULL;
}

static bool hff_run(hff_t *s)
{
    AVPacket *packet;
    bool ok;

    packet = av_packet_alloc();
    if (packet == NULL) {
        return false;
    }

    ok = true;

    while (ok && av_read_frame(s->in, packet) >= 0) {
        if (packet->stream_index == s->video) {
            ok = hff_decode(s, packet);
        } else if (s->map[packet->stream_index] >= 0) {
            ok = hff_copy(s, packet);
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    ok = ok && hff_decode(s, NULL);

    if (ok && s->session == NULL) {
        fprintf(stderr, "hdrg-ffmpeg: no video frames\n");

        return false;
    }

    ok = ok && hff_status("the end", hdrg_session_send_frame(s->session, NULL)) && hff_drain(s);
    ok = ok && s->enc != NULL && hff_encode(s, NULL);

    if (ok && av_write_trailer(s->out) < 0) {
        fprintf(stderr, "hdrg-ffmpeg: the trailer could not be written\n");

        return false;
    }

    return ok;
}

static void hff_close(hff_t *s)
{
    uint32_t i, k;

    hdrg_session_destroy(s->session);
    hdrg_context_destroy(s->ctx);
    avcodec_free_context(&s->dec);
    avcodec_free_context(&s->enc);
    av_frame_free(&s->next_map);
    av_frame_free(&s->next);
    av_buffer_pool_uninit(&s->out_pool);
    av_buffer_pool_uninit(&s->sws_pool);
    av_buffer_unref(&s->hw_frames);
    av_buffer_unref(&s->hw_device);
    sws_freeContext(s->sws);
    avformat_close_input(&s->in);

    if (s->out != NULL && (s->out->oformat->flags & AVFMT_NOFILE) == 0) {
        avio_closep(&s->out->pb);
    }

    avformat_free_context(s->out);

    for (i = 0; i < HFF_RING; i++) {
        av_frame_free(&s->props[i]);
    }

    for (i = 0; i < s->queue_count; i++) {
        av_packet_free(&s->queued[i]);
    }

    for (i = 0; i < HFF_META; i++) {
        for (k = 0; k < HFF_META_KINDS; k++) {
            av_buffer_unref(&s->meta[i][k]);
        }
    }

    av_dict_free(&s->enc_opts);
    free(s->map);
}

int main(int argc, char **argv)
{
    const char *input_path, *output_path, *preset, *temporal;
    hdrg_context_desc_t desc;
    hdrg_status_t status;
    hff_t s;
    uint8_t preset_id;
    int i;
    bool ok;

    memset(&s, 0, sizeof(s));
    input_path = NULL;
    output_path = NULL;
    preset = "photo";
    temporal = "windowed";
    s.audio = true;
    s.max_frames = UINT64_MAX;
    s.target = AV_PIX_FMT_NONE;
    hdrg_session_config_init(&s.config);
    hdrg_display_caps_init(&s.caps);

    /* a file nobody watches meanwhile: never late */
    s.config.latency = HDRG_LATENCY_OFFLINE;

    /* the preset first: --set adjusts it */
    for (i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--preset") == 0) {
            preset = argv[i + 1];
        }
    }

    if (!tools_named("--preset", "preset", preset, &preset_id)) {
        return 2;
    }

    hdrg_params_init(&s.params, preset_id);
    ok = true;

    for (i = 1; ok && i < argc; i++) {
        if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--encoder") == 0 && i + 1 < argc) {
            s.encoder = argv[++i];
        } else if (strcmp(argv[i], "--enc-opt") == 0 && i + 1 < argc) {
            ok = av_dict_parse_string(&s.enc_opts, argv[++i], "=", ":", 0) >= 0;
        } else if (strcmp(argv[i], "--max-cll") == 0 && i + 1 < argc) {
            ok = sscanf(argv[++i], "%f,%f", &s.given_cll, &s.given_fall) == 2;
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            s.audio = false;
        } else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
            ok = tools_set(argv[++i], &s.params, &s.caps, NULL, &s.config);
        } else if (strcmp(argv[i], "--temporal") == 0 && i + 1 < argc) {
            temporal = argv[++i];
        } else if (strcmp(argv[i], "--past") == 0 && i + 1 < argc) {
            s.config.past = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--future") == 0 && i + 1 < argc) {
            s.config.future = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--proxy") == 0 && i + 1 < argc) {
            s.config.proxy = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-flash-limit") == 0) {
            s.config.flash_off = true;
        } else if (strcmp(argv[i], "--no-dither") == 0) {
            s.config.dither_off = true;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            s.threads = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            s.gpu = (strcmp(argv[++i], "auto") == 0)    ? HDRG_GPU_AUTO
                    : (strcmp(argv[i], "filters") == 0) ? HDRG_GPU_FILTERS
                                                        : HDRG_GPU_OFF;
        } else if (strcmp(argv[i], "--gpu-analysis") == 0 && i + 1 < argc) {
            s.gpu_analysis =
                (strcmp(argv[++i], "fast") == 0) ? HDRG_GPU_ANALYSIS_FAST : HDRG_GPU_ANALYSIS_EXACT;
        } else if (strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            ok = tools_named("--latency", "latency", argv[++i], &s.config.latency);
        } else if (strcmp(argv[i], "--decoder-grain") == 0) {
            s.decoder_grain = true;
        } else if (strcmp(argv[i], "--hwaccel") == 0 && i + 1 < argc) {
            s.hwaccel = strcmp(argv[++i], "videotoolbox") == 0;
            ok = s.hwaccel;
        } else if (strcmp(argv[i], "--out-rate") == 0 && i + 1 < argc) {
            ok = sscanf(argv[++i], "%d:%d", &s.out_rate.num, &s.out_rate.den) == 2 &&
                 s.out_rate.num > 0 && s.out_rate.den > 0;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            s.max_frames = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (input_path == NULL) {
            input_path = argv[i];
        } else if (output_path == NULL) {
            output_path = argv[i];
        } else {
            ok = false;
        }
    }

    /* (TWO_PASS reads its input twice: hdrg-video's) */
    if (ok && (!tools_named("--temporal", "temporal", temporal, &s.config.temporal) ||
               s.config.temporal == HDRG_TEMPORAL_TWO_PASS)) {
        ok = false;
    }

    if (!ok || input_path == NULL || output_path == NULL) {
        usage();
        av_dict_free(&s.enc_opts);

        return 2;
    }

    /* VideoToolbox's pixel buffers on the GPU need the library's GPU work */
    s.vt_out = s.encoder != NULL && strstr(s.encoder, "_videotoolbox") != NULL;
    s.gpu = (s.hwaccel && s.gpu == HDRG_GPU_OFF) ? HDRG_GPU_AUTO : s.gpu;

    hdrg_context_desc_init(&desc);
    desc.thread_count = s.threads;
    desc.gpu = s.gpu;
    desc.gpu_analysis = s.gpu_analysis;
    status = hdrg_context_create(&desc, &s.ctx);

    ok = hff_status("the context", status) && hff_open_input(&s, input_path) &&
         hff_open_output(&s, output_path) && hff_run(&s);

    if (ok) {
        fprintf(stderr,
                "%llu frames; measured MaxCLL %.0f, MaxFALL %.0f cd/m^2; mastering display "
                "Display P3 D65, 0.005 to %.0f cd/m^2\n",
                (unsigned long long)s.frames, (double)s.max_cll, (double)s.max_fall,
                (double)s.peak);
    }

    hff_close(&s);

    return ok ? 0 : 1;
}
