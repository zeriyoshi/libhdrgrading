/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * The VapourSynth plugin: hdrg.Grade(clip, ...) grades an SDR
 * clip of 8 to 16-bit integer Y'CbCr or R'G'B' into single-layer PQ through a
 * session: 4:2:0 10-bit Y'CbCr of the BT.2020 container (default) or 16-bit
 * R'G'B'. It uses the public headers only.
 *
 * Frames come in any order and from several requests at a time (fmUnordered
 * runs one call at a time, in no order). A frame's output reads the source
 * frames from hdrg_session_preroll() before it to its window's future, so a
 * request next to the session's position advances the session (the frames
 * passed on the way go to the cache), and any other opens a new one that
 * many frames back: each frame equals the output of one session over the
 * whole clip (RECURSIVE: approximately). Each call requests the source
 * frames its plan needs; when another call has moved the session in the
 * meantime, it requests the rest and waits again.
 *
 * fpsnum / fpsden give the output another frame rate (the clip's needed):
 * output frame j at the clip's time j x rate / out_rate, the frames between
 * interpolated by the session; frame j reads from the source frame at or
 * before that time, its properties the nearer source frame's. The session
 * takes the source frames' pts as their numbers x HDRG_VS_PTS, so each
 * output's number comes back from its pts.
 *
 * A frame carries its dynamic metadata as T.35 payloads when measured:
 * HDR10Plus (hdrg_hdr10plus_t35, FFMS2's property) and HdrgST2094_50
 * (hdrg_st2094_50_t35, FFmpeg's bytes), binary.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <VapourSynth4.h>

#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/version.h"
#include "hdrgrading/video.h"

#define HDRG_VS_ID        "io.github.zeriyoshi.hdrgrading"
#define HDRG_VS_ARGS_MAX  4096u
#define HDRG_VS_NAME_MAX  64u
#define HDRG_VS_SLOTS     (2u * HDRG_SESSION_MAX_WINDOW + 2u)
#define HDRG_VS_PTS       65536 /* a source frame's pts: its number x this */
#define HDRG_VS_MIN(a, b) (((a) < (b)) ? (a) : (b))
#define HDRG_VS_MAX(a, b) (((a) > (b)) ? (a) : (b))

/* the mastering display: Display P3 (D65) from 0.005 cd/m^2 up to the rendition peak */
#define HDRG_VS_MASTER_MIN 0.005

/* frameData: the requested source frames [lo, hi], kept as n + 1 (0 = none yet) */
#define HDRG_VS_LO 0
#define HDRG_VS_HI 1

typedef struct {
    const VSAPI *vsapi;
    VSNode *node;
    VSVideoInfo vi; /* the output's */
    hdrg_context_t *ctx;
    hdrg_session_t *session;
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_session_config_t config;
    hdrg_session_format_t format;        /* the session's frames */
    VSMap *props[HDRG_VS_SLOTS];         /* the sent frames' properties, by n % slots (made once) */
    uint8_t *rgb;                        /* R'G'B' input: slots interleaved frames */
    uint8_t *packed;                     /* R'G'B' output: the session's interleaved rows */
    size_t frame_bytes;                  /* an interleaved input frame's */
    uint32_t preroll, ahead, slots;      /* ahead: the frames a call advances through */
    uint32_t lag;                        /* the frames an output waits for (hdrg_session_latency) */
    uint64_t num, den;                   /* output j at source time j num / den (reduced) */
    float peak;                          /* the mastering display's (hdrg_session_peak) */
    int32_t frames, next_out, next_send; /* the source's; next_send past frames: the end sent */
    int32_t kept;                        /* the first source frame that may still be held */
    int32_t matrix, transfer, primaries; /* < 0: the frames' (or the default) */
    int32_t range;                       /* < 0: the frames'; 0 full, 1 limited */
    int32_t chromaloc;                   /* < 0: the frames'; HDRG_CHROMA_* */
    bool held[HDRG_VS_SLOTS];            /* props[n % slots] is a frame's not output yet */
    bool settled;                        /* the colors are known */
    bool yuv_in, rgb_out;
    bool interp; /* another output rate */
} hdrg_vs_t;

static char hdrg_vs_args[HDRG_VS_ARGS_MAX]; /* hdrg.Grade's arguments, built once */

/* the source frame at or before output j's time */
static int32_t hdrg_vs_source_of(const hdrg_vs_t *vs, int32_t j)
{
    return (int32_t)((uint64_t)j * vs->num / vs->den);
}

/* the first output at or after source frame t */
static int32_t hdrg_vs_output_of(const hdrg_vs_t *vs, int32_t t)
{
    return (int32_t)(((uint64_t)t * vs->den + vs->num - 1u) / vs->num);
}

/* the number of a session's output of pts (the source frames' x HDRG_VS_PTS) */
static int32_t hdrg_vs_index(const hdrg_vs_t *vs, int64_t pts)
{
    return (int32_t)(((uint64_t)pts * vs->den + vs->num * (HDRG_VS_PTS / 2)) /
                     (vs->num * HDRG_VS_PTS));
}

static uint64_t hdrg_vs_gcd(uint64_t a, uint64_t b)
{
    uint64_t t;

    while (b != 0) {
        t = a % b;
        a = b;
        b = t;
    }

    return a;
}

/* a slot's properties no output needs any longer (the map kept for the next frame's) */
static void hdrg_vs_unhold(hdrg_vs_t *vs, uint32_t slot)
{
    if (vs->props[slot] != NULL) {
        vs->vsapi->clearMap(vs->props[slot]);
    }

    vs->held[slot] = false;
}

static void hdrg_vs_close(hdrg_vs_t *vs)
{
    uint32_t i;

    hdrg_session_destroy(vs->session);
    vs->session = NULL;

    for (i = 0; i < HDRG_VS_SLOTS; i++) {
        hdrg_vs_unhold(vs, i);
    }
}

/* the held properties no output needs any longer: those before the next output's source frame */
static void hdrg_vs_drop(hdrg_vs_t *vs)
{
    const int32_t needed = hdrg_vs_source_of(vs, vs->next_out);

    for (; vs->kept < needed && vs->kept < vs->next_send; vs->kept++) {
        hdrg_vs_unhold(vs, (uint32_t)vs->kept % vs->slots);
    }
}

/* an H.273 property of a frame; unset or unspecified (2) gives fallback */
static int32_t hdrg_vs_prop(const VSAPI *vsapi, const VSMap *props, const char *key,
                            int32_t fallback)
{
    int64_t value;
    int error;

    value = vsapi->mapGetInt(props, key, 0, &error);

    return (error != peSuccess || value == 2 || value < 0 || value > 255) ? fallback
                                                                          : (int32_t)value;
}

/* the colors from the arguments, else the frame's properties, else the defaults */
static void hdrg_vs_settle(hdrg_vs_t *vs, const VSFrame *frame)
{
    const VSMap *props = vs->vsapi->getFramePropertiesRO(frame);
    int32_t primaries, transfer, matrix, range, chroma;
    int error;

    primaries = (vs->primaries >= 0)
                    ? vs->primaries
                    : hdrg_vs_prop(vs->vsapi, props, "_Primaries", HDRG_PRIMARIES_BT709);
    transfer = (vs->transfer >= 0)
                   ? vs->transfer
                   : hdrg_vs_prop(vs->vsapi, props, "_Transfer",
                                  vs->yuv_in ? HDRG_TRANSFER_BT709 : HDRG_TRANSFER_SRGB);
    matrix = (vs->matrix >= 0) ? vs->matrix : hdrg_vs_prop(vs->vsapi, props, "_Matrix", 0);
    range = vs->range;

    if (range < 0) {
        range = (int32_t)vs->vsapi->mapGetInt(props, "_ColorRange", 0, &error);
        range = (error != peSuccess) ? 1 : range;
    }

    /* not hdrg_vs_prop: 2 is a location (top-left) */
    chroma = vs->chromaloc;

    if (chroma < 0) {
        chroma = (int32_t)vs->vsapi->mapGetInt(props, "_ChromaLocation", 0, &error);
        chroma = (error != peSuccess || chroma < 0 || chroma > HDRG_CHROMA_BOTTOM)
                     ? HDRG_CHROMA_LEFT
                     : chroma;
    }

    if (matrix == 0) {
        matrix = (primaries == HDRG_PRIMARIES_BT2020) ? HDRG_MATRIX_BT2020
                 : (primaries == HDRG_PRIMARIES_BT601_625 || primaries == HDRG_PRIMARIES_BT601_525)
                     ? HDRG_MATRIX_BT601
                     : HDRG_MATRIX_BT709;
    }

    /* BT.470 BG and SMPTE 170M share the matrix */
    matrix = (matrix == 5) ? HDRG_MATRIX_BT601 : matrix;

    vs->format.primaries = (uint8_t)primaries;
    vs->format.transfer = (uint8_t)transfer;
    vs->config.input.matrix = (uint8_t)matrix;
    vs->config.input.full_range = range == 0;
    vs->config.input.chroma_loc = (uint8_t)chroma;
    vs->settled = true;
}

/*
 * a session from source frame start, rendering from output n's source
 * frame (the frames before only build its state)
 */
static hdrg_status_t hdrg_vs_open(hdrg_vs_t *vs, int32_t start, int32_t n)
{
    const int32_t source = hdrg_vs_source_of(vs, n);
    hdrg_session_config_t config;

    hdrg_vs_close(vs);

    config = vs->config;
    config.start = (uint32_t)start;
    config.preroll = (uint32_t)(source - start);
    vs->next_send = start;
    vs->next_out = hdrg_vs_output_of(vs, source);
    vs->kept = hdrg_vs_source_of(vs, vs->next_out);

    return hdrg_session_create(vs->ctx, &vs->format, &vs->params, &vs->caps, &config, &vs->session);
}

/*
 * The source frames output n needs, [*first, *last] (empty when *first >
 * *last): the rest of its window when the session is at most ahead frames
 * before it, else a new session's (true)
 */
static bool hdrg_vs_plan(const hdrg_vs_t *vs, int32_t n, int32_t *first, int32_t *last)
{
    const int32_t source = hdrg_vs_source_of(vs, n);

    *last = HDRG_VS_MIN(source + (int32_t)vs->lag + (vs->interp ? 1 : 0), vs->frames - 1);

    if (vs->session != NULL && n >= vs->next_out && n - vs->next_out <= (int32_t)vs->ahead) {
        *first = vs->next_send;

        return false;
    }

    *first = (source > (int32_t)vs->preroll) ? source - (int32_t)vs->preroll : 0;

    return true;
}

/*
 * Source frame t into the session, then freed: the session reads Y'CbCr at
 * the send and R'G'B' from its interleaved copy; its properties are copied
 * for its output's unless a preroll's
 */
static hdrg_status_t hdrg_vs_send(hdrg_vs_t *vs, const VSFrame *src, int32_t t)
{
    const VSAPI *vsapi = vs->vsapi;
    const uint8_t *row;
    hdrg_session_frame_t in;
    hdrg_status_t status;
    uint32_t x, y, w, h, slot, bits, value;
    uint16_t *wide;
    uint8_t p, *dst;

    memset(&in, 0, sizeof(in));
    in.pts = (int64_t)t * HDRG_VS_PTS;
    slot = (uint32_t)t % vs->slots;

    if (vs->yuv_in) {
        for (p = 0; p < 3u; p++) {
            in.planes[p] = vsapi->getReadPtr(src, p);
            in.strides[p] = (size_t)vsapi->getStride(src, p);
        }
    } else {
        /* planar R'G'B' interleaved: 8 bits as they are, more widened to 16 */
        w = (uint32_t)vs->vi.width;
        h = (uint32_t)vs->vi.height;
        bits = (uint32_t)vsapi->getVideoFrameFormat(src)->bitsPerSample;
        dst = vs->rgb + (size_t)slot * vs->frame_bytes;

        for (p = 0; p < 3u; p++) {
            for (y = 0; y < h; y++) {
                row = vsapi->getReadPtr(src, p) + (size_t)y * (size_t)vsapi->getStride(src, p);

                if (bits == 8) {
                    for (x = 0; x < w; x++) {
                        dst[((size_t)y * w + x) * 3u + p] = row[x];
                    }

                    continue;
                }

                wide = (uint16_t *)(void *)dst;
                for (x = 0; x < w; x++) {
                    value = ((const uint16_t *)(const void *)row)[x];
                    wide[((size_t)y * w + x) * 3u + p] =
                        (uint16_t)((value << (16u - bits)) | (value >> (2u * bits - 16u)));
                }
            }
        }

        in.planes[0] = dst;
    }

    status = hdrg_session_send_frame(vs->session, &in);
    if (status != HDRG_OK) {
        vsapi->freeFrame(src);

        return status;
    }

    if (t >= hdrg_vs_source_of(vs, vs->next_out)) {
        if (vs->props[slot] == NULL) {
            vs->props[slot] = vsapi->createMap();
        }

        vsapi->clearMap(vs->props[slot]);
        vsapi->copyMap(vsapi->getFramePropertiesRO(src), vs->props[slot]);
        vs->held[slot] = true;
    }

    vsapi->freeFrame(src);
    vs->next_send = t + 1;

    return HDRG_OK;
}

/* the output frame's planes for the session, at out */
static void hdrg_vs_planes(const hdrg_vs_t *vs, VSFrame *dst, hdrg_session_output_t *out)
{
    uint8_t p;

    memset(out, 0, sizeof(*out));

    if (vs->rgb_out) {
        out->planes[0] = vs->packed;
        out->strides[0] = (size_t)vs->vi.width * 6u;

        return;
    }

    for (p = 0; p < 3u; p++) {
        out->planes[p] = vs->vsapi->getWritePtr(dst, p);
        out->strides[p] = (size_t)vs->vsapi->getStride(dst, p);
    }
}

/* the output's colors, mastering display and statistics, over the source frame's properties */
static void hdrg_vs_props(const hdrg_vs_t *vs, const VSMap *src, VSFrame *dst,
                          const hdrg_session_output_t *out)
{
    const VSAPI *vsapi = vs->vsapi;
    const double xs[3] = {0.680, 0.265, 0.150}, ys[3] = {0.320, 0.690, 0.060};
    VSMap *props = vsapi->getFramePropertiesRW(dst);
    uint8_t plus[HDRG_HDR10PLUS_T35_BYTES], curve[HDRG_ST2094_50_T35_MAX];
    size_t size;

    if (src != NULL) {
        vsapi->copyMap(src, props);
    }

    vsapi->mapSetInt(props, "_Matrix", vs->rgb_out ? 0 : HDRG_MATRIX_BT2020, maReplace);
    vsapi->mapSetInt(props, "_Primaries",
                     (vs->caps.gamut == HDRG_GAMUT_BT2020) ? HDRG_PRIMARIES_BT2020
                                                           : HDRG_PRIMARIES_P3_D65,
                     maReplace);
    vsapi->mapSetInt(props, "_Transfer", 16, maReplace);
    vsapi->mapSetInt(props, "_ColorRange", vs->rgb_out ? 0 : 1, maReplace);

    if (vs->rgb_out) {
        vsapi->mapDeleteKey(props, "_ChromaLocation");
    } else {
        vsapi->mapSetInt(props, "_ChromaLocation", 0, maReplace);
    }

    vsapi->mapSetFloatArray(props, "MasteringDisplayPrimariesX", xs, 3);
    vsapi->mapSetFloatArray(props, "MasteringDisplayPrimariesY", ys, 3);
    vsapi->mapSetFloat(props, "MasteringDisplayWhitePointX", 0.3127, maReplace);
    vsapi->mapSetFloat(props, "MasteringDisplayWhitePointY", 0.3290, maReplace);
    vsapi->mapSetFloat(props, "MasteringDisplayMinLuminance", HDRG_VS_MASTER_MIN, maReplace);
    vsapi->mapSetFloat(props, "MasteringDisplayMaxLuminance", (double)vs->peak, maReplace);
    vsapi->mapSetFloat(props, "HdrgMaxNits", (double)out->max_nits, maReplace);
    vsapi->mapSetFloat(props, "HdrgAvgNits", (double)out->avg_nits, maReplace);
    vsapi->mapSetInt(props, "HdrgFlashLimited", out->flash_limited ? 1 : 0, maReplace);
    vsapi->mapSetInt(props, "_SceneChangePrev", out->scene_cut ? 1 : 0, maReplace);

    /* the dynamic metadata's T.35 payloads: HDR10+ as FFMS2's HDR10Plus, ST 2094-50 (FFmpeg's) */
    size = hdrg_hdr10plus_t35(&out->hdr10plus, plus, sizeof(plus));
    if (size > 0) {
        vsapi->mapSetData(props, "HDR10Plus", (const char *)plus, (int)size, dtBinary, maReplace);
    } else {
        vsapi->mapDeleteKey(props, "HDR10Plus");
    }

    size = hdrg_st2094_50_t35(&out->st2094_50, curve, sizeof(curve));
    if (size > 0) {
        vsapi->mapSetData(props, "HdrgST2094_50", (const char *)curve, (int)size, dtBinary,
                          maReplace);
    } else {
        vsapi->mapDeleteKey(props, "HdrgST2094_50");
    }

    /* another output rate: the output's frame length, whether it is between the clip's */
    if (vs->interp) {
        vsapi->mapSetInt(props, "_DurationNum", vs->vi.fpsDen, maReplace);
        vsapi->mapSetInt(props, "_DurationDen", vs->vi.fpsNum, maReplace);
        vsapi->mapSetInt(props, "HdrgSynthetic", out->synthetic ? 1 : 0, maReplace);
        vsapi->mapDeleteKey(props, "_AbsoluteTime");
    }
}

/* R'G'B' output: the session's interleaved rows into the frame's planes */
static void hdrg_vs_unpack(const hdrg_vs_t *vs, VSFrame *dst)
{
    const uint16_t *src = (const uint16_t *)(const void *)vs->packed;
    uint32_t x, y;
    uint16_t *row;
    uint8_t p;

    for (p = 0; p < 3u; p++) {
        for (y = 0; y < (uint32_t)vs->vi.height; y++) {
            row = (uint16_t *)(void *)(vs->vsapi->getWritePtr(dst, p) +
                                       (size_t)y * (size_t)vs->vsapi->getStride(dst, p));

            for (x = 0; x < (uint32_t)vs->vi.width; x++) {
                row[x] = src[((size_t)y * (uint32_t)vs->vi.width + x) * 3u + p];
            }
        }
    }
}

/*
 * Frame n from the session, a new one when restart (from frame first). The
 * frames on the way to it go to the cache; the source frames it reads were
 * requested by this call
 */
static const VSFrame *hdrg_vs_render(hdrg_vs_t *vs, int32_t n, bool restart, int32_t first,
                                     VSFrameContext *frame_ctx, VSCore *core)
{
    const VSAPI *vsapi = vs->vsapi;
    const VSFrame *src, *result;
    hdrg_session_output_t out;
    hdrg_status_t status;
    VSFrame *dst;
    uint32_t slot;
    int32_t j, source;
    char message[160];

    status = HDRG_OK;

    if (!vs->settled) {
        src = vsapi->getFrameFilter(hdrg_vs_source_of(vs, n), vs->node, frame_ctx);
        hdrg_vs_settle(vs, src);
        vsapi->freeFrame(src);
    }

    if (restart) {
        status = hdrg_vs_open(vs, first, n);
    }

    result = NULL;
    dst = NULL;

    while (status == HDRG_OK && vs->next_out <= n) {
        if (dst == NULL) {
            dst = vsapi->newVideoFrame(&vs->vi.format, vs->vi.width, vs->vi.height, NULL, core);
        }

        hdrg_vs_planes(vs, dst, &out);

        status = hdrg_session_receive_frame(vs->session, &out);
        if (status == HDRG_ERR_AGAIN) {
            if (vs->next_send < vs->frames) {
                src = vsapi->getFrameFilter(vs->next_send, vs->node, frame_ctx);
                status = (src != NULL) ? hdrg_vs_send(vs, src, vs->next_send) : HDRG_ERR_INTERNAL;
            } else {
                status = hdrg_session_send_frame(vs->session, NULL);
                vs->next_send = vs->frames + 1;
            }

            continue;
        }

        if (status != HDRG_OK) {
            break;
        }

        /* the output's number from its pts, its properties the (nearer) source frame's */
        j = hdrg_vs_index(vs, out.pts);
        source = (int32_t)((!out.synthetic ? out.pts : out.src_pts[(out.phase < 32768u) ? 0 : 1]) /
                           HDRG_VS_PTS);

        if (vs->rgb_out) {
            hdrg_vs_unpack(vs, dst);
        }

        slot = (uint32_t)source % vs->slots;
        hdrg_vs_props(vs, vs->held[slot] ? vs->props[slot] : NULL, dst, &out);

        if (j == n) {
            result = dst;
        } else {
            vsapi->cacheFrame(dst, j, frame_ctx);
            vsapi->freeFrame(dst);
        }

        dst = NULL;
        vs->next_out = j + 1;
        hdrg_vs_drop(vs);
    }

    if (dst != NULL) {
        vsapi->freeFrame(dst);
    }

    if (status != HDRG_OK) {
        hdrg_vs_close(vs);
        snprintf(message, sizeof(message), "hdrg.Grade: frame %d: %s", (int)n,
                 hdrg_status_string(status));
        vsapi->setFilterError(message, frame_ctx);

        return NULL;
    }

    return result;
}

static const VSFrame *VS_CC hdrg_vs_get_frame(int n, int reason, void *instance, void **frame_data,
                                              VSFrameContext *frame_ctx, VSCore *core,
                                              const VSAPI *vsapi)
{
    hdrg_vs_t *vs = (hdrg_vs_t *)instance;
    int32_t first, last, lo, hi, t;
    bool restart;

    if (reason != arInitial && reason != arAllFramesReady) {
        return NULL;
    }

    restart = hdrg_vs_plan(vs, n, &first, &last);

    /* n's source frame always, for the colors (a request with no frames would never return) */
    first = HDRG_VS_MIN(first, hdrg_vs_source_of(vs, n));

    if (reason == arInitial) {
        for (t = first; t <= last; t++) {
            vsapi->requestFrameFilter(t, vs->node, frame_ctx);
        }

        frame_data[HDRG_VS_LO] = (void *)(intptr_t)(first + 1);
        frame_data[HDRG_VS_HI] = (void *)(intptr_t)(last + 1);

        return NULL;
    }

    lo = (int32_t)(intptr_t)frame_data[HDRG_VS_LO] - 1;
    hi = (int32_t)(intptr_t)frame_data[HDRG_VS_HI] - 1;

    /* another call moved the session since: the frames this plan needs besides */
    if (first < lo || last > hi) {
        for (t = first; t < lo; t++) {
            vsapi->requestFrameFilter(t, vs->node, frame_ctx);
        }

        for (t = hi + 1; t <= last; t++) {
            vsapi->requestFrameFilter(t, vs->node, frame_ctx);
        }

        frame_data[HDRG_VS_LO] = (void *)(intptr_t)(HDRG_VS_MIN(first, lo) + 1);
        frame_data[HDRG_VS_HI] = (void *)(intptr_t)(HDRG_VS_MAX(last, hi) + 1);

        return NULL;
    }

    return hdrg_vs_render(vs, n, restart, first, frame_ctx, core);
}

static void VS_CC hdrg_vs_free(void *instance, VSCore *core, const VSAPI *vsapi)
{
    hdrg_vs_t *vs = (hdrg_vs_t *)instance;
    uint32_t i;

    (void)core;

    hdrg_vs_close(vs);

    for (i = 0; i < HDRG_VS_SLOTS; i++) {
        if (vs->props[i] != NULL) {
            vsapi->freeMap(vs->props[i]);
        }
    }

    vsapi->freeNode(vs->node);
    hdrg_context_destroy(vs->ctx);
    free(vs->rgb);
    free(vs->packed);
    free(vs);
}

/* a setting's argument name: the dashes as underscores */
static void hdrg_vs_arg_name(const char *name, char *out)
{
    size_t i;

    for (i = 0; name[i] != '\0' && i + 1u < HDRG_VS_NAME_MAX; i++) {
        out[i] = (name[i] == '-') ? '_' : name[i];
    }

    out[i] = '\0';
}

/* the content intent and display settings given as arguments */
static bool hdrg_vs_settings(hdrg_vs_t *vs, const VSMap *in, const VSAPI *vsapi, VSMap *out)
{
    hdrg_setting_t setting;
    uint32_t i;
    int error;
    char arg[HDRG_VS_NAME_MAX], message[160];
    double value;

    for (i = 0; hdrg_setting_at(i, &setting); i++) {
        /* (the preset and a session's are the filter's own arguments) */
        if (setting.part == HDRG_SETTING_ENCODE || setting.part == HDRG_SETTING_SESSION ||
            strcmp(setting.name, "preset") == 0) {
            continue;
        }

        hdrg_vs_arg_name(setting.name, arg);

        if (vsapi->mapNumElements(in, arg) <= 0) {
            continue;
        }

        value = (setting.kind == HDRG_SETTING_FLOAT) ? vsapi->mapGetFloat(in, arg, 0, &error)
                                                     : (double)vsapi->mapGetInt(in, arg, 0, &error);

        if (hdrg_setting_set(setting.name, value, &vs->params, &vs->caps, NULL) != HDRG_OK) {
            snprintf(message, sizeof(message), "hdrg.Grade: %s: invalid value", arg);
            vsapi->mapSetError(out, message);

            return false;
        }
    }

    return true;
}

/* an optional integer argument */
static int64_t hdrg_vs_int(const VSMap *in, const char *key, int64_t fallback, const VSAPI *vsapi)
{
    int64_t value;
    int error;

    value = vsapi->mapGetInt(in, key, 0, &error);

    return (error != peSuccess) ? fallback : value;
}

/* the session's configuration and the output format from the arguments; NULL or an error */
static const char *hdrg_vs_configure(hdrg_vs_t *vs, const VSMap *in, const VSAPI *vsapi)
{
    const char *preset, *temporal, *format;
    int error;
    double value;

    preset = vsapi->mapGetData(in, "preset", 0, &error);
    preset = (error != peSuccess) ? "photo" : preset;
    temporal = vsapi->mapGetData(in, "temporal", 0, &error);
    temporal = (error != peSuccess) ? "windowed" : temporal;
    format = vsapi->mapGetData(in, "format", 0, &error);
    format = (error != peSuccess) ? "yuv420p10" : format;

    /* the library's names (hdrg_setting_parse) */
    if (hdrg_setting_parse("preset", preset, &value) != HDRG_OK ||
        hdrg_setting_set("preset", value, &vs->params, NULL, NULL) != HDRG_OK) {
        return "preset must be photo, anime or game";
    }

    hdrg_display_caps_init(&vs->caps);
    hdrg_session_config_init(&vs->config);
    vs->config.latency = HDRG_LATENCY_OFFLINE;

    /* (TWO_PASS reads the clip twice: not a filter's) */
    if (hdrg_setting_parse("temporal", temporal, &value) != HDRG_OK ||
        value == (double)HDRG_TEMPORAL_TWO_PASS ||
        hdrg_session_setting_set("temporal", value, &vs->config) != HDRG_OK) {
        return "temporal must be windowed, recursive or static";
    }

    vs->config.past = (uint16_t)HDRG_VS_MAX(
        0, HDRG_VS_MIN(hdrg_vs_int(in, "past", vs->config.past, vsapi), 65535));
    vs->config.future = (hdrg_vs_int(in, "future", -1, vsapi) < 0)
                            ? HDRG_SESSION_AUTO
                            : (uint16_t)HDRG_VS_MIN(hdrg_vs_int(in, "future", 0, vsapi), 65535);
    vs->config.proxy = (uint16_t)HDRG_VS_MAX(
        0, HDRG_VS_MIN(hdrg_vs_int(in, "proxy", vs->config.proxy, vsapi), 65535));
    vs->config.flash_off = hdrg_vs_int(in, "flash_limit", 1, vsapi) == 0;
    vs->config.dither_off = hdrg_vs_int(in, "dither", 1, vsapi) == 0;

    if (strcmp(format, "yuv420p10") == 0) {
        vs->rgb_out = false;
        vs->caps.gamut = HDRG_GAMUT_BT2020;
        vs->config.output.layout = HDRG_YUV_420;
        vs->config.output.matrix = HDRG_MATRIX_BT2020;
        vs->config.output.bits = 10;
        vs->config.output.full_range = false;
    } else if (strcmp(format, "rgb48") == 0) {
        vs->rgb_out = true;
        vs->caps.gamut =
            (hdrg_vs_int(in, "bt2020", 0, vsapi) != 0) ? HDRG_GAMUT_BT2020 : HDRG_GAMUT_P3;
        vs->config.output.layout = 0;
        vs->config.output.bits = 16;
    } else {
        return "format must be yuv420p10 or rgb48";
    }

    vs->matrix = (int32_t)hdrg_vs_int(in, "matrix", -1, vsapi);
    vs->transfer = (int32_t)hdrg_vs_int(in, "transfer", -1, vsapi);
    vs->primaries = (int32_t)hdrg_vs_int(in, "primaries", -1, vsapi);
    vs->range = (int32_t)hdrg_vs_int(in, "range", -1, vsapi);
    vs->chromaloc = (int32_t)hdrg_vs_int(in, "chromaloc", -1, vsapi);

    if (vs->chromaloc > HDRG_CHROMA_BOTTOM) {
        return "chromaloc must be 0 to 5";
    }

    /* the clip's rate (the flash limiter's second, the fades), another output's */
    if (vs->vi.fpsNum > 0 && vs->vi.fpsDen > 0) {
        vs->config.rate_num = (uint32_t)vs->vi.fpsNum;
        vs->config.rate_den = (uint32_t)vs->vi.fpsDen;
    }

    if (hdrg_vs_int(in, "fpsnum", 0, vsapi) != 0) {
        if (vs->config.rate_num == 0) {
            return "fpsnum needs the clip's frame rate";
        }

        if (hdrg_vs_int(in, "fpsnum", 0, vsapi) < 1 || hdrg_vs_int(in, "fpsden", 1, vsapi) < 1 ||
            hdrg_vs_int(in, "fpsnum", 0, vsapi) > INT32_MAX ||
            hdrg_vs_int(in, "fpsden", 1, vsapi) > INT32_MAX) {
            return "fpsnum and fpsden must be positive";
        }

        vs->config.out_rate_num = (uint32_t)hdrg_vs_int(in, "fpsnum", 0, vsapi);
        vs->config.out_rate_den = (uint32_t)hdrg_vs_int(in, "fpsden", 1, vsapi);
        vs->interp = true;
    }

    return NULL;
}

/* the source clip's format into the session's; NULL or an error */
static const char *hdrg_vs_source(hdrg_vs_t *vs, const VSVideoInfo *vi)
{
    const VSVideoFormat *f = &vi->format;
    uint64_t gcd;

    if (f->colorFamily == 0 || vi->width == 0 || vi->height == 0 || vi->numFrames <= 0) {
        return "the clip needs a constant format and size";
    }

    if (f->sampleType != stInteger || f->bitsPerSample < 8 || f->bitsPerSample > 16 ||
        (f->colorFamily != cfYUV && f->colorFamily != cfRGB)) {
        return "the clip must be 8 to 16-bit integer YUV or RGB";
    }

    if (f->colorFamily == cfYUV && !((f->subSamplingW == 1 && f->subSamplingH == 1) ||
                                     (f->subSamplingW == 1 && f->subSamplingH == 0) ||
                                     (f->subSamplingW == 0 && f->subSamplingH == 0))) {
        return "YUV must be 4:2:0, 4:2:2 or 4:4:4";
    }

    vs->yuv_in = f->colorFamily == cfYUV;
    vs->frames = vi->numFrames;
    vs->num = 1;
    vs->den = 1;

    /* another output rate: output j at source time j rate / out_rate, none after the last frame */
    if (vs->interp) {
        vs->num = (uint64_t)vs->config.rate_num * vs->config.out_rate_den;
        vs->den = (uint64_t)vs->config.rate_den * vs->config.out_rate_num;
        gcd = hdrg_vs_gcd(vs->num, vs->den);
        vs->num /= gcd;
        vs->den /= gcd;

        if ((uint64_t)(vi->numFrames - 1) * vs->den / vs->num + 1u > INT32_MAX) {
            return "the output would have too many frames";
        }

        vs->vi.numFrames = (int)((uint64_t)(vi->numFrames - 1) * vs->den / vs->num + 1u);
        vs->vi.fpsNum = (int64_t)vs->config.out_rate_num;
        vs->vi.fpsDen = (int64_t)vs->config.out_rate_den;
    }

    memset(&vs->format, 0, sizeof(vs->format));
    vs->format.width = (uint32_t)vi->width;
    vs->format.height = (uint32_t)vi->height;
    vs->format.primaries = HDRG_PRIMARIES_BT709;
    vs->format.transfer = HDRG_TRANSFER_BT709;
    vs->config.input.layout = 0;

    if (vs->yuv_in) {
        vs->config.input.layout = (f->subSamplingH == 1)
                                      ? HDRG_YUV_420
                                      : ((f->subSamplingW == 1) ? HDRG_YUV_422 : HDRG_YUV_444);
        vs->config.input.matrix = HDRG_MATRIX_BT709;
        vs->config.input.bits = (uint8_t)f->bitsPerSample;
    } else {
        vs->format.bit_depth = (f->bitsPerSample == 8) ? 8u : 16u;
        vs->format.stride = (size_t)vi->width * 3u * (vs->format.bit_depth / 8u);
    }

    return NULL;
}

/*
 * hdrg.Grade: the arguments into a context and a probe session (its
 * settings checked, its preroll read), the output clip
 */
static void VS_CC hdrg_vs_create(const VSMap *in, VSMap *out, void *user, VSCore *core,
                                 const VSAPI *vsapi)
{
    const char *problem;
    VSFilterDependency deps[1];
    hdrg_context_desc_t desc;
    hdrg_session_format_t probe_format;
    hdrg_session_t *probe;
    hdrg_status_t status;
    hdrg_vs_t *vs;
    VSNode *node;
    int error;
    char message[200];

    (void)user;

    vs = (hdrg_vs_t *)calloc(1, sizeof(*vs));
    if (vs == NULL) {
        vsapi->mapSetError(out, "hdrg.Grade: out of memory");

        return;
    }

    vs->vsapi = vsapi;
    vs->node = vsapi->mapGetNode(in, "clip", 0, &error);
    vs->vi = *vsapi->getVideoInfo(vs->node);

    problem = hdrg_vs_configure(vs, in, vsapi);
    if (problem == NULL) {
        problem = hdrg_vs_source(vs, &vs->vi);
    }

    if (problem == NULL && !hdrg_vs_settings(vs, in, vsapi, out)) {
        hdrg_vs_free(vs, core, vsapi);

        return;
    }

    if (problem != NULL) {
        snprintf(message, sizeof(message), "hdrg.Grade: %s", problem);
        vsapi->mapSetError(out, message);
        hdrg_vs_free(vs, core, vsapi);

        return;
    }

    hdrg_context_desc_init(&desc);
    desc.thread_count = (uint32_t)HDRG_VS_MAX(0, hdrg_vs_int(in, "threads", 0, vsapi));
    status = hdrg_context_create(&desc, &vs->ctx);

    /* the probe: the settings, at the clip's size (the colors are settled at the first frame) */
    probe = NULL;
    probe_format = vs->format;

    if (status == HDRG_OK) {
        status = hdrg_session_create(vs->ctx, &probe_format, &vs->params, &vs->caps, &vs->config,
                                     &probe);
    }

    if (status == HDRG_OK) {
        vs->preroll = hdrg_session_preroll(probe);
        vs->lag = hdrg_session_latency(probe);
        vs->peak = hdrg_session_peak(probe);
        hdrg_session_destroy(probe);
    }

    vs->slots = vs->lag + (vs->interp ? 2u : 1u);

    if (status == HDRG_OK && vs->slots > HDRG_VS_SLOTS) {
        status = HDRG_ERR_UNSUPPORTED;
    }

    vs->frame_bytes = vs->format.stride * vs->format.height;

    /* the session took the clip's size: the frames of the window may still pass size_t */
    if (status == HDRG_OK && !vs->yuv_in && vs->frame_bytes > SIZE_MAX / vs->slots) {
        status = HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && !vs->yuv_in) {
        vs->rgb = (uint8_t *)malloc(vs->frame_bytes * vs->slots);
        status = (vs->rgb != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && vs->rgb_out) {
        vs->packed = (uint8_t *)malloc((size_t)vs->vi.width * (size_t)vs->vi.height * 6u);
        status = (vs->packed != NULL) ? HDRG_OK : HDRG_ERR_NOMEM;
    }

    if (status != HDRG_OK) {
        snprintf(message, sizeof(message), "hdrg.Grade: %s", hdrg_status_string(status));
        vsapi->mapSetError(out, message);
        hdrg_vs_free(vs, core, vsapi);

        return;
    }

    if (vs->rgb_out) {
        vsapi->queryVideoFormat(&vs->vi.format, cfRGB, stInteger, 16, 0, 0, core);
    } else {
        vsapi->queryVideoFormat(&vs->vi.format, cfYUV, stInteger, 10, 1, 1, core);
    }

    deps[0].source = vs->node;
    deps[0].requestPattern = rpGeneral;
    node = vsapi->createVideoFilter2("Grade", &vs->vi, hdrg_vs_get_frame, hdrg_vs_free, fmUnordered,
                                     deps, 1, vs, core);
    if (node == NULL) {
        vsapi->mapSetError(out, "hdrg.Grade: the filter could not be created");

        return;
    }

    /* the frames a call advances through go to the cache */
    vs->ahead = (uint32_t)HDRG_VS_MAX(vsapi->setLinearFilter(node), 0);
    vsapi->mapConsumeNode(out, "clip", node, maReplace);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi)
{
    hdrg_setting_t setting;
    uint32_t i;
    size_t used;
    char arg[HDRG_VS_NAME_MAX];

    vspapi->configPlugin(HDRG_VS_ID, "hdrg", "libhdrgrading: SDR to HDR grading",
                         VS_MAKE_VERSION(HDRG_VERSION_MAJOR, HDRG_VERSION_MINOR),
                         VAPOURSYNTH_API_VERSION, 0, plugin);

    used = (size_t)snprintf(hdrg_vs_args, sizeof(hdrg_vs_args),
                            "clip:vnode;preset:data:opt;temporal:data:opt;past:int:opt;"
                            "future:int:opt;proxy:int:opt;flash_limit:int:opt;dither:int:opt;"
                            "format:data:opt;bt2020:int:opt;matrix:int:opt;transfer:int:opt;"
                            "primaries:int:opt;range:int:opt;chromaloc:int:opt;threads:int:opt;"
                            "fpsnum:int:opt;fpsden:int:opt;");

    for (i = 0; hdrg_setting_at(i, &setting) && used < sizeof(hdrg_vs_args); i++) {
        if (setting.part == HDRG_SETTING_ENCODE || setting.part == HDRG_SETTING_SESSION ||
            strcmp(setting.name, "preset") == 0) {
            continue;
        }

        hdrg_vs_arg_name(setting.name, arg);
        used += (size_t)snprintf(hdrg_vs_args + used, sizeof(hdrg_vs_args) - used, "%s:%s:opt;",
                                 arg, (setting.kind == HDRG_SETTING_FLOAT) ? "float" : "int");
    }

    vspapi->registerFunction("Grade", hdrg_vs_args, "clip:vnode;", hdrg_vs_create, NULL, plugin);
}
