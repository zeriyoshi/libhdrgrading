/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "common.h"
#include "hdrgrading/hdrgrading.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"

#define VIDEO_LINE     1024u
#define VIDEO_MAX_SETS 16u

/* a y4m stream's frames */
typedef struct {
    uint32_t width, height, rate_num, rate_den;
    uint8_t layout, bits, chroma_loc;
    bool full_range;
} video_y4m_t;

/* a frame's pixels: held from its read until the session releases them */
typedef struct {
    uint8_t *pixels;
    bool held;
} video_slot_t;

/*
 * The input: its file, the slots its frames are read into (made when every
 * one is held: the frames the session reads raw R'G'B' of until their
 * output, one when it copies them at the send) and their layout
 */
typedef struct {
    FILE *file;
    video_slot_t **slots;
    size_t bytes, offsets[3], strides[3];
    uint64_t max_frames;
    uint32_t count; /* slots */
    uint8_t layout; /* 0: raw RGB */
    bool failed;    /* a slot could not be made */
} video_input_t;

/*
 * The sidecars, written as the frames come out: HDR10+'s JSON (hdr10plus_tool's
 * and x265's --dhdr10-info, profile A: no tone mapping curve) and ST
 * 2094-50's (the library's), both from the bytes of the T.35 payloads
 */
typedef struct {
    FILE *hdr10plus, *st2094_50;
    uint64_t *scenes; /* the first frame of each scene */
    uint64_t scene_frame;
    size_t scene_count, scene_room;
} video_sidecar_t;

/*
 * Video through a session: y4m (Y'CbCr) or
 * raw RGB frames in, single-layer PQ frames out as raw RGB or y4m, so FFmpeg
 * can sit on both ends of a pipe without being linked. --stats writes the
 * per-frame statistics (for flicker measurements).
 */
static void usage(void)
{
    fprintf(stderr,
            "usage: hdrg-video [--size <W>x<H>] [options] <input|-> <output|->\n"
            "       hdrg-video --export-lut <file.cube> [--lut-size <n>] [options]\n"
            "\n"
            "input:  y4m (YUV4MPEG2: 4:2:0 / 4:2:2 / 4:4:4, 8 to 16 bits, its XCOLORRANGE),\n"
            "        or raw RGB frames with --size: 8-bit (rgb24) or 16-bit little-endian\n"
            "        with --depth 16 (rgb48le). BT.709 primaries and the sRGB curve unless\n"
            "        --primaries / --transfer (ITU-T H.273 code points) say otherwise\n"
            "output: SMPTE ST 2084 (PQ), one frame per input frame: raw RGB, 8-bit (rgb24)\n"
            "        or 16-bit (rgb48le with --out-depth 16), Display P3 primaries or a\n"
            "        BT.2020 container with --bt2020 (the colors stay within P3); or with\n"
            "        --y4m, y4m 4:2:0 10-bit of the BT.2020 container and matrix\n"
            "        (non-constant luminance), limited range. At the end the measured\n"
            "        MaxCLL / MaxFALL and the mastering display are printed as x265\n"
            "        options, and the delay a frame took (its causes in frames, at the\n"
            "        rate and with the measured work). For example:\n"
            "          ffmpeg -i in.mp4 -f yuv4mpegpipe -strict -1 - |\n"
            "            hdrg-video --y4m - - |\n"
            "            ffmpeg -i - -color_primaries bt2020 -color_trc smpte2084 \\\n"
            "              -colorspace bt2020nc -c:v libx265 -x265-params <options> out.mp4\n"
            "\n");
    fprintf(
        stderr,
        "options:\n"
        "  --size <W>x<H>        raw RGB input of this frame size\n"
        "  --depth <8|16>        raw RGB input sample bits (default 8)\n"
        "  --primaries <n>       H.273 colour primaries of the input (default 1)\n"
        "  --transfer <n>        H.273 transfer characteristics of the input (default 13)\n"
        "  --matrix <n>          H.273 matrix coefficients of y4m input: 1, 5, 6 or 9\n"
        "                        (default 9 with BT.2020 primaries, 6 with 5 or 6, else 1)\n"
        "  --chroma-loc <0..5>   H.273 chroma location of y4m input (default: its C tag,\n"
        "                        420jpeg / 420 = 1, 420mpeg2 = 0, 420paldv = 2, others 0)\n"
        "  --chroma-fit <fit>    how y4m input's chroma follows the luma: linear or\n"
        "                        segmented (bent at an outline between two fills;\n"
        "                        preset default: anime)\n"
        "  --rate <num:den>      frame rate of raw input (default 30:1): of the y4m\n"
        "                        output and of the recursive mode's time constants\n"
        "  --out-rate <num:den>  the output's frame rate: the frames between the input's\n"
        "                        from the interp filter (motion compensated by default)\n"
        "  --preset <name>       photo (default), anime or game\n"
        "  --diffuse-max <1..4>  how far diffuse content rises, x SDR (default 2)\n"

        "  --strength <0..1>     global strength\n"
        "  --denoise <0..4>      the PREFILTER stage's spatial noise reduction, its threshold\n"
        "                        over the measured noise (1 = the default strength), the grain\n"
        "                        reduced; without the option photo removes the grain and\n"
        "                        resynthesizes it in PQ as SDR showed it, anime / game keep\n"
        "                        it, not amplified (--set grain-policy=0..3: preserve,\n"
        "                        reduce, resynth, auto)\n"
        "  --deblock <0..4>      the PREFILTER stage's deblocking of the 8-pixel grid, its\n"
        "                        thresholds over the measured block step (0 = off; preset\n"
        "                        default: photo / anime 1)\n"
        "  --dering <0..3>       the deringing after it, its window's radius 1 + 2 x this\n"
        "                        (0 = off; anime 2)\n"
        "  --defringe <0..3>     the defringing after it (the false color around edges),\n"
        "                        its window's radius 1 + 2 x this (0 = off; anime 3)\n"
        "  --deband <0..3>       the debanding after it (8-bit steps in gradients), its\n"
        "                        residual threshold x this (0 = off; photo 0.5, anime 2.5)\n"
        "  --mctf <0..4>         the temporal noise reduction after it (2 frames each way\n"
        "                        along the motion; 2 frames of delay, none under --latency\n"
        "                        low), its threshold x this (0 = off; photo / anime 1)\n"
        "  --filter <stage>=<id> use filter <id> on a stage: scale, prefilter (repeated:\n"
        "                        a chain in that order, instead of the built-in one; e.g.\n"
        "                        prefilter=hdrg.webgpu.deband on the GPU) or interp\n"
        "                        (the frames between, --out-rate: hdrg.interp.mc (default),\n"
        "                        hdrg.interp.blend or hdrg.interp.repeat)\n"
        "  --filter-opt <k>=<v>  an option of the preceding --filter\n");
    fprintf(stderr,
            "  --effort <e>          realtime (lights judged on 4x4 cells, the analysis on a\n"
            "                        reduction) or quality (finer cells, full resolution, no\n"
            "                        time budget); default quality with --latency offline,\n"
            "                        else realtime\n"
            "  --threads <n>         0 = auto (default), 1 = single\n"
            "  --gpu <mode>          auto: the built-in prefilters and Lanczos with a GPU\n"
            "                        version and the apply pass on the GPU (Metal, else\n"
            "                        Vulkan, else WebGPU) when there is one; filters: the\n"
            "                        filters alone (the CPU's bytes but for deband / NR);\n"
            "                        default off\n"
            "  --gpu-analysis <a>    with --gpu auto, where the analysis adds up its pixels:\n"
            "                        exact (the CPU's statistics bit for bit, default) or fast\n"
            "                        (the sums on the GPU, within its tolerance)\n"
            "  --shed                playback / low: once late, the frames after pass the\n"
            "                        prefilters without a time window through (reported)\n"
            "  --no-dither           no output dithering (for frame-to-frame comparisons)\n"
            "  --no-flash-limit      no flash limiter (ITU-R BT.1702-3: the gain of a rise that\n"
            "                        would make more than three harmful flashes a second is\n"
            "                        lowered)\n"
            "  --out-depth <8|16>    raw RGB output sample bits (default 8)\n"
            "  --bt2020              write a BT.2020 container\n"
            "  --y4m                 write y4m 4:2:0 10-bit (implies --bt2020)\n"
            "  --temporal <mode>     how the scene statistics follow the frames: windowed\n"
            "                        (default), recursive (an EMA, no latency), two-pass\n"
            "                        (constant over each scene of a first pass; an input\n"
            "                        file, not -) or static (no statistics: one fixed mapping)\n"
            "  --export-lut <file>   write the static mapping as a .cube 3D LUT (input: the\n"
            "                        source's R'G'B' codes; output: PQ R'G'B') and exit\n"
            "  --lut-size <n>        nodes per axis of --export-lut (default 33, 2..129)\n"
            "  --past <n>            windowed / two-pass: frames before a frame its statistics\n"
            "                        average over (default 8, 0 = every frame on its own; a\n"
            "                        build without the temporal filters: 0 only)\n"
            "  --future <n>          windowed: frames after it (the latency; default the\n"
            "                        past's, low 0)\n"
            "  --latency <profile>   offline (default: converting a file nobody watches,\n"
            "                        never late), playback (watched: keeps up on average, the\n"
            "                        ASYNC filters keep their frames in flight) or low (the\n"
            "                        low-latency mode: each frame within its interval, each\n"
            "                        job waited for at once, no --future); the report at the\n"
            "                        end says whether frames came late and what to change\n"
            "  --output-size <W>x<H> output pixels through the SCALE stage (one side 0 = keep\n"
            "                        the aspect ratio; default the input's)\n"
            "  --upscaler <name>     the built-in scaler of an output larger on both sides:\n"
            "                        lanczos, edge, fsr1 or nis (preset default: photo\n"
            "                        lanczos, anime / game edge); any other size is lanczos\n"
            "  --proxy <n>           analyze frames on a reduction whose longest side is at\n"
            "                        most n (default 960, 0 = at full resolution)\n"
            "  --frames <n>          stop after n frames\n"
            "  --set <name=value>    any hdrg-cli setting after the preset and the options\n"
            "                        above (clarity=0, colored-lights=0, ...)\n"
            "  --stats <file.csv>    per-frame statistics\n"
            "  --hdr10plus-json <file>  each frame's HDR10+ as hdr10plus_tool's JSON (x265's\n"
            "                        --dhdr10-info; profile A: no tone mapping curve)\n"
            "  --st2094-50-json <file>  each frame's ST 2094-50 gain curve: its fields and its\n"
            "                        T.35 payload in hex (after the header B500900001)\n");
}

static double video_now(void)
{
    struct timespec ts;

    (void)timespec_get(&ts, TIME_UTC);

    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static FILE *video_open(const char *path, const char *mode, FILE *standard)
{
    if (strcmp(path, "-") != 0) {
        return fopen(path, mode);
    }

#ifdef _WIN32
    (void)_setmode(_fileno(standard), _O_BINARY);
#endif

    return standard;
}

static const char *video_profile_name(uint8_t profile)
{
    const char *name = hdrg_setting_value_name("latency", (double)profile);

    return (name != NULL) ? name : "?";
}

/* a part's GPU in the work's breakdown (0: the CPU's, none) */
static const char *video_device_name(uint32_t device)
{
    return (device == HDRG_DEVICE_METAL)    ? " (Metal)"
           : (device == HDRG_DEVICE_VULKAN) ? " (Vulkan)"
           : (device == HDRG_DEVICE_WEBGPU) ? " (WebGPU)"
                                            : "";
}

/* the frames that came late and what the session advises */
static void video_advice(const hdrg_session_latency_t *latency)
{
    fprintf(stderr, "%u frames late: the session does not keep up at %.3f fps\n",
            latency->late_frames, (double)latency->rate);

    if ((latency->advice & HDRG_ADVICE_PREFILTER) != 0) {
        fprintf(stderr, "  the prefilters take the most: fewer or lighter ones, or the GPU "
                        "versions (--gpu auto)\n");
    }

    if ((latency->advice & HDRG_ADVICE_EFFORT) != 0) {
        fprintf(stderr, "  the analysis takes the most: --effort realtime\n");
    }

    if ((latency->advice & HDRG_ADVICE_PROXY) != 0) {
        fprintf(stderr, "  the analysis takes the most: a smaller --proxy\n");
    }

    if ((latency->advice & HDRG_ADVICE_SCALE) != 0) {
        fprintf(stderr, "  the scaling takes the most: a smaller --output-size\n");
    }

    if ((latency->advice & HDRG_ADVICE_THREADS) != 0) {
        fprintf(stderr, "  more --threads (fewer than the CPUs)\n");
    }

    if ((latency->advice & HDRG_ADVICE_LATENCY) != 0) {
        fprintf(stderr, "  --latency playback keeps the GPU jobs in flight (frames of delay)\n");
    }
}

/* a header line without its newline: false when it does not fit or the input ends */
static bool video_line(FILE *file, char *line, size_t size)
{
    size_t n;

    if (fgets(line, (int)size, file) == NULL) {
        return false;
    }

    n = strlen(line);
    if (n == 0 || line[n - 1u] != '\n') {
        return false;
    }

    line[n - 1u] = '\0';

    return true;
}

/*
 * the C tag of a y4m stream: 420 / 422 / 444, then nothing (8 bits) or
 * p9..p16; 4:2:0 at 8 bits names its chroma siting (none: jpeg)
 */
static bool video_y4m_colorspace(const char *tag, video_y4m_t *y4m)
{
    const char *rest;
    uint32_t bits;

    if (strncmp(tag, "420", 3) == 0) {
        y4m->layout = HDRG_YUV_420;
    } else if (strncmp(tag, "422", 3) == 0) {
        y4m->layout = HDRG_YUV_422;
    } else if (strncmp(tag, "444", 3) == 0) {
        y4m->layout = HDRG_YUV_444;
    } else {
        return false;
    }

    rest = tag + 3;
    y4m->bits = 8;

    if (rest[0] == 'p') {
        bits = (uint32_t)strtoul(rest + 1, NULL, 10);
        if (bits < 9u || bits > 16u) {
            return false;
        }

        y4m->bits = (uint8_t)bits;

        return true;
    }

    if (y4m->layout != HDRG_YUV_420) {
        return rest[0] == '\0';
    }

    y4m->chroma_loc = HDRG_CHROMA_CENTER;

    if (strcmp(rest, "paldv") == 0) {
        y4m->chroma_loc = HDRG_CHROMA_TOPLEFT;
    } else if (strcmp(rest, "mpeg2") == 0) {
        y4m->chroma_loc = HDRG_CHROMA_LEFT;
    }

    return rest[0] == '\0' || strcmp(rest, "jpeg") == 0 || strcmp(rest, "paldv") == 0 ||
           strcmp(rest, "mpeg2") == 0;
}

static bool video_y4m_header(FILE *file, video_y4m_t *y4m)
{
    char line[VIDEO_LINE], *token, *end;

    memset(y4m, 0, sizeof(*y4m));
    y4m->rate_num = 30;
    y4m->rate_den = 1;
    y4m->layout = HDRG_YUV_420;
    y4m->bits = 8;

    if (!video_line(file, line, sizeof(line)) || strncmp(line, "YUV4MPEG2 ", 10) != 0) {
        return false;
    }

    token = line + 10;
    while (token != NULL) {
        end = strchr(token, ' ');
        if (end != NULL) {
            *end = '\0';
        }

        if (token[0] == 'W') {
            y4m->width = (uint32_t)strtoul(token + 1, NULL, 10);
        } else if (token[0] == 'H') {
            y4m->height = (uint32_t)strtoul(token + 1, NULL, 10);
        } else if (token[0] == 'F') {
            if (sscanf(token + 1, "%u:%u", &y4m->rate_num, &y4m->rate_den) != 2) {
                return false;
            }
        } else if (token[0] == 'C') {
            if (!video_y4m_colorspace(token + 1, y4m)) {
                return false;
            }
        } else if (strcmp(token, "XCOLORRANGE=FULL") == 0) {
            y4m->full_range = true;
        }

        token = (end != NULL) ? end + 1 : NULL;
    }

    return y4m->width > 0 && y4m->height > 0 && y4m->rate_num > 0 && y4m->rate_den > 0;
}

/* a y4m frame's header: false at the end of the stream */
static bool video_y4m_frame(FILE *file)
{
    char line[VIDEO_LINE];

    return video_line(file, line, sizeof(line)) && strncmp(line, "FRAME", 5) == 0;
}

/* the offsets and strides of a y4m frame's planes; returns its bytes */
static size_t video_y4m_layout(const video_y4m_t *y4m, size_t offsets[3], size_t strides[3])
{
    const uint32_t cw = (y4m->layout == HDRG_YUV_444) ? y4m->width : (y4m->width + 1u) / 2u,
                   ch = (y4m->layout == HDRG_YUV_420) ? (y4m->height + 1u) / 2u : y4m->height;
    const size_t bytes = (y4m->bits > 8u) ? 2u : 1u;

    strides[0] = (size_t)y4m->width * bytes;
    strides[1] = (size_t)cw * bytes;
    strides[2] = strides[1];
    offsets[0] = 0;
    offsets[1] = strides[0] * y4m->height;
    offsets[2] = offsets[1] + strides[1] * ch;

    return offsets[2] + strides[2] * ch;
}

static void video_release(void *opaque)
{
    ((video_slot_t *)opaque)->held = false;
}

/* a slot no frame holds, made when every one is (NULL: out of memory, input->failed) */
static video_slot_t *video_slot(video_input_t *input)
{
    video_slot_t **grown, *slot;
    uint32_t i;

    for (i = 0; i < input->count; i++) {
        if (!input->slots[i]->held) {
            return input->slots[i];
        }
    }

    grown = (video_slot_t **)realloc(input->slots, (input->count + 1u) * sizeof(*grown));
    slot = (video_slot_t *)calloc(1, sizeof(*slot));

    if (grown != NULL) {
        input->slots = grown;
    }

    if (grown == NULL || slot == NULL || (slot->pixels = (uint8_t *)malloc(input->bytes)) == NULL) {
        free(slot);
        input->failed = true;

        return NULL;
    }

    input->slots[input->count++] = slot;

    return slot;
}

/* frame n of the input into a slot as a session frame: false at the end or at max_frames */
static bool video_next(video_input_t *input, uint64_t n, hdrg_session_frame_t *in)
{
    video_slot_t *slot;
    uint8_t p;

    slot = (n < input->max_frames) ? video_slot(input) : NULL;
    if (slot == NULL) {
        return false;
    }

    memset(in, 0, sizeof(*in));
    in->planes[0] = slot->pixels;
    in->pts = (int64_t)n;
    in->release = video_release;
    in->opaque = slot;

    for (p = 0; input->layout != 0 && p < 3u; p++) {
        in->planes[p] = slot->pixels + input->offsets[p];
        in->strides[p] = input->strides[p];
    }

    slot->held = (input->layout == 0 || video_y4m_frame(input->file)) &&
                 fread(slot->pixels, 1, input->bytes, input->file) == input->bytes;

    return slot->held;
}

static void video_input_free(video_input_t *input)
{
    uint32_t i;

    for (i = 0; i < input->count; i++) {
        free(input->slots[i]->pixels);
        free(input->slots[i]);
    }

    free(input->slots);
}

/*
 * TWO_PASS: the first pass over the input from where its frames start (a
 * record a frame into *passes, their lights into *lights, both malloc'ed),
 * then back to that start
 */
static hdrg_status_t video_first_pass(hdrg_context_t *ctx, const hdrg_session_format_t *format,
                                      const hdrg_params_t *params, const hdrg_display_caps_t *caps,
                                      const hdrg_session_config_t *window, video_input_t *input,
                                      hdrg_session_pass_t **passes, hdrg_session_light_t **lights,
                                      uint32_t *count)
{
    hdrg_session_config_t first;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_pass_t *grown;
    hdrg_session_light_t *more;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint64_t sent;
    uint32_t capacity, i;
    int64_t start;
    size_t judged, room;
    bool ended;

    first = *window;
    first.passes = NULL;
    first.pass_count = 0;
    *passes = NULL;
    *lights = NULL;
    *count = 0;
    capacity = 0;
    judged = 0;
    room = 0;
    start = (int64_t)ftell(input->file);

    status = hdrg_session_create(ctx, format, params, caps, &first, &session);
    if (status != HDRG_OK) {
        return status;
    }

    sent = 0;
    ended = false;
    for (;;) {
        memset(&out, 0, sizeof(out));

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN && !ended) {
            if (video_next(input, sent, &in)) {
                status = hdrg_session_send_frame(session, &in);
                sent++;
            } else if (input->failed) {
                status = HDRG_ERR_NOMEM;
            } else {
                status = hdrg_session_send_frame(session, NULL);
                ended = true;
            }

            if (status == HDRG_OK) {
                continue;
            }
        }

        if (status != HDRG_OK) {
            break;
        }

        if (*count == capacity) {
            capacity = capacity * 2u + 64u;
            grown = (hdrg_session_pass_t *)realloc(*passes, capacity * sizeof(**passes));
            if (grown == NULL) {
                status = HDRG_ERR_NOMEM;

                break;
            }

            *passes = grown;
        }

        if (judged + out.light_count > room) {
            room = (judged + out.light_count) * 2u;
            more = (hdrg_session_light_t *)realloc(*lights, room * sizeof(**lights));
            if (more == NULL) {
                status = HDRG_ERR_NOMEM;

                break;
            }

            *lights = more;
        }

        if (out.light_count > 0u) {
            memcpy(*lights + judged, out.lights, out.light_count * sizeof(**lights));
        }

        judged += out.light_count;
        (*passes)[*count].lights = NULL;
        (*passes)[*count].light_count = out.light_count;
        (*passes)[*count].anchor_luma = out.anchor_luma;
        (*passes)[*count].key_luma = out.key_luma;
        (*passes)[*count].apl_fit = out.apl_fit;
        (*passes)[*count].scene_cut = out.scene_cut;
        (*count)++;
    }

    hdrg_session_destroy(session);

    if (status != HDRG_EOF) {
        return status;
    }

    /* the lights move while they grow: their records' pointers once they are all in */
    judged = 0;

    for (i = 0; i < *count; i++) {
        (*passes)[i].lights = ((*passes)[i].light_count > 0u) ? *lights + judged : NULL;
        judged += (*passes)[i].light_count;
    }

    return (*count > 0 && start >= 0 && fseek(input->file, (long)start, SEEK_SET) == 0)
               ? HDRG_OK
               : HDRG_ERR_INVALID_ARG;
}

/* n bits of p at bit *at, the most significant first */
static uint32_t video_bits(const uint8_t *p, uint32_t *at, uint8_t n)
{
    uint32_t v;
    uint8_t i;

    v = 0;

    for (i = 0; i < n; i++) {
        v = (v << 1) | ((uint32_t)(p[*at >> 3] >> (7u - (*at & 7u))) & 1u);
        (*at)++;
    }

    return v;
}

static uint16_t video_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* the u16 list of p's count values as a JSON array */
static bool video_u16s(FILE *file, const uint8_t *p, uint8_t count)
{
    uint8_t i;
    bool written;

    written = fputc('[', file) != EOF;

    for (i = 0; written && i < count; i++) {
        written = fprintf(file, "%s%u", (i > 0) ? ", " : "", (unsigned)video_be16(p + i * 2u)) > 0;
    }

    return written && fputc(']', file) != EOF;
}

/* the sidecars' files (NULL paths: none) and their heads */
static bool video_sidecar_open(video_sidecar_t *sidecar, const char *hdr10plus_path,
                               const char *st2094_50_path)
{
    memset(sidecar, 0, sizeof(*sidecar));

    if (hdr10plus_path != NULL) {
        sidecar->hdr10plus = fopen(hdr10plus_path, "w");
        if (sidecar->hdr10plus == NULL ||
            fputs("{\n  \"JSONInfo\": {\n    \"HDR10plusProfile\": \"A\",\n    \"Version\": "
                  "\"1.0\"\n  },\n  \"SceneInfo\": [",
                  sidecar->hdr10plus) == EOF) {
            return false;
        }
    }

    if (st2094_50_path != NULL) {
        sidecar->st2094_50 = fopen(st2094_50_path, "w");
        if (sidecar->st2094_50 == NULL ||
            fprintf(sidecar->st2094_50,
                    "{\n  \"Format\": \"SMPTE ST 2094-50\",\n  \"Version\": 1,\n  "
                    "\"T35Header\": \"B500900001\",\n  \"ToolInfo\": {\n    \"Tool\": "
                    "\"libhdrgrading\",\n    \"Version\": \"%s\"\n  },\n  \"Frames\": [",
                    hdrg_version()) < 0) {
            return false;
        }
    }

    return true;
}

/* frame's HDR10+ entry: the values of its T.35 payload (zeros when not measured) */
static bool video_sidecar_hdr10plus(const video_sidecar_t *sidecar,
                                    const hdrg_session_output_t *out, uint64_t frame)
{
    hdrg_hdr10plus_t plus;
    uint32_t at, maxscl[3], average, values[HDRG_HDR10PLUS_PERCENTILES];
    uint8_t payload[HDRG_HDR10PLUS_T35_BYTES], indexes[HDRG_HDR10PLUS_PERCENTILES], c;
    bool written;

    plus = out->hdr10plus;

    if (!plus.valid) {
        memset(&plus, 0, sizeof(plus));
        plus.valid = true;
    }

    (void)hdrg_hdr10plus_t35(&plus, payload, sizeof(payload));
    at = 8u + 2u + 27u + 1u;

    for (c = 0; c < 3u; c++) {
        maxscl[c] = video_bits(payload, &at, 17);
    }

    average = video_bits(payload, &at, 17);
    at += 4u;

    for (c = 0; c < HDRG_HDR10PLUS_PERCENTILES; c++) {
        indexes[c] = (uint8_t)video_bits(payload, &at, 7);
        values[c] = video_bits(payload, &at, 17);
    }

    written = fprintf(sidecar->hdr10plus,
                      "%s\n    {\n      \"LuminanceParameters\": {\n        \"AverageRGB\": %u,\n"
                      "        \"LuminanceDistributions\": {\n          \"DistributionIndex\": [",
                      (frame > 0) ? "," : "", (unsigned)average) > 0;

    for (c = 0; written && c < HDRG_HDR10PLUS_PERCENTILES; c++) {
        written =
            fprintf(sidecar->hdr10plus, "%s%u", (c > 0) ? ", " : "", (unsigned)indexes[c]) > 0;
    }

    written =
        written && fputs("],\n          \"DistributionValues\": [", sidecar->hdr10plus) != EOF;

    for (c = 0; written && c < HDRG_HDR10PLUS_PERCENTILES; c++) {
        written = fprintf(sidecar->hdr10plus, "%s%u", (c > 0) ? ", " : "", (unsigned)values[c]) > 0;
    }

    return written &&
           fprintf(
               sidecar->hdr10plus,
               "]\n        },\n        \"MaxScl\": [%u, %u, %u]\n      },\n"
               "      \"NumberOfWindows\": 1,\n      \"TargetedSystemDisplayMaximumLuminance\": "
               "0,\n      \"SceneFrameIndex\": %llu,\n      \"SceneId\": %llu,\n"
               "      \"SequenceFrameIndex\": %llu\n    }",
               (unsigned)maxscl[0], (unsigned)maxscl[1], (unsigned)maxscl[2],
               (unsigned long long)sidecar->scene_frame,
               (unsigned long long)(sidecar->scene_count - 1u), (unsigned long long)frame) > 0;
}

/*
 * frame's ST 2094-50 entry: the fields of its T.35 payload (their units: the
 * standard's) and the payload's bytes; none fitted: the payload null
 */
static bool video_sidecar_st2094_50(const video_sidecar_t *sidecar,
                                    const hdrg_session_output_t *out, uint64_t frame)
{
    const uint8_t *p;
    uint8_t payload[HDRG_ST2094_50_T35_MAX], alternates, a, count, pchip;
    size_t size, k;
    bool written;

    size = hdrg_st2094_50_t35(&out->st2094_50, payload, sizeof(payload));
    written = fprintf(sidecar->st2094_50,
                      "%s\n    {\n      \"SequenceFrameIndex\": %llu,\n      \"Pts\": %lld,\n"
                      "      \"SceneId\": %llu,\n",
                      (frame > 0) ? "," : "", (unsigned long long)frame, (long long)out->pts,
                      (unsigned long long)(sidecar->scene_count - 1u)) > 0;

    if (size == 0) {
        return written && fputs("      \"T35\": null\n    }", sidecar->st2094_50) != EOF;
    }

    p = payload + 2;
    written = written && fprintf(sidecar->st2094_50, "      \"HdrReferenceWhite\": %u,\n",
                                 ((payload[1] & 0x80u) != 0) ? (unsigned)video_be16(p) : 1015u) > 0;
    p += ((payload[1] & 0x80u) != 0) ? 2u : 0u;
    alternates = (uint8_t)((p[2] >> 4) & 0x07u);
    written = written && fprintf(sidecar->st2094_50,
                                 "      \"BaselineHdrHeadroom\": %u,\n"
                                 "      \"GainApplicationSpaceChromaticities\": %u,\n"
                                 "      \"Alternates\": [",
                                 (unsigned)video_be16(p), (unsigned)((p[2] >> 2) & 0x03u)) > 0;
    p += 3;

    for (a = 0; written && a < alternates; a++) {
        count = (uint8_t)((p[3] >> 3) + 1u);
        pchip = (uint8_t)((p[3] >> 2) & 1u);
        written = fprintf(sidecar->st2094_50,
                          "%s\n        {\n          \"AlternateHdrHeadroom\": %u,\n"
                          "          \"ComponentMixingType\": %u,\n"
                          "          \"GainCurveUsePchipSlope\": %s,\n"
                          "          \"GainCurveControlPointsX\": ",
                          (a > 0) ? "," : "", (unsigned)video_be16(p), (unsigned)(p[2] >> 6),
                          pchip ? "true" : "false") > 0;
        p += 4;
        written = written && video_u16s(sidecar->st2094_50, p, count) &&
                  fputs(",\n          \"GainCurveControlPointsY\": ", sidecar->st2094_50) != EOF &&
                  video_u16s(sidecar->st2094_50, p + count * 2u, count) &&
                  fputs("\n        }", sidecar->st2094_50) != EOF;
        p += (size_t)count * (pchip ? 4u : 6u);
    }

    written = written && fputs("\n      ],\n      \"T35\": \"", sidecar->st2094_50) != EOF;

    for (k = 0; written && k < size; k++) {
        written = fprintf(sidecar->st2094_50, "%02X", (unsigned)payload[k]) > 0;
    }

    return written && fputs("\"\n    }", sidecar->st2094_50) != EOF;
}

/* the frame's entries in the sidecars (a scene cut starts a scene) */
static bool video_sidecar_frame(video_sidecar_t *sidecar, const hdrg_session_output_t *out,
                                uint64_t frame)
{
    uint64_t *grown;

    if (sidecar->hdr10plus == NULL && sidecar->st2094_50 == NULL) {
        return true;
    }

    if (frame == 0 || out->scene_cut) {
        if (sidecar->scene_count == sidecar->scene_room) {
            sidecar->scene_room = (sidecar->scene_room > 0) ? sidecar->scene_room * 2u : 64u;
            grown = (uint64_t *)realloc(sidecar->scenes, sidecar->scene_room * sizeof(uint64_t));
            if (grown == NULL) {
                return false;
            }

            sidecar->scenes = grown;
        }

        sidecar->scenes[sidecar->scene_count++] = frame;
        sidecar->scene_frame = 0;
    } else {
        sidecar->scene_frame++;
    }

    return (sidecar->hdr10plus == NULL || video_sidecar_hdr10plus(sidecar, out, frame)) &&
           (sidecar->st2094_50 == NULL || video_sidecar_st2094_50(sidecar, out, frame));
}

/* the sidecars' tails (HDR10+'s scenes) after frames frames; closes them */
static bool video_sidecar_close(video_sidecar_t *sidecar, uint64_t frames)
{
    uint64_t next;
    size_t s;
    bool written;

    written = true;

    if (sidecar->hdr10plus != NULL) {
        written = fputs("\n  ],\n  \"SceneInfoSummary\": {\n    \"SceneFirstFrameIndex\": [",
                        sidecar->hdr10plus) != EOF;

        for (s = 0; written && s < sidecar->scene_count; s++) {
            written = fprintf(sidecar->hdr10plus, "%s%llu", (s > 0) ? ", " : "",
                              (unsigned long long)sidecar->scenes[s]) > 0;
        }

        written = written && fputs("],\n    \"SceneFrameNumbers\": [", sidecar->hdr10plus) != EOF;

        for (s = 0; written && s < sidecar->scene_count; s++) {
            next = (s + 1u < sidecar->scene_count) ? sidecar->scenes[s + 1u] : frames;
            written = fprintf(sidecar->hdr10plus, "%s%llu", (s > 0) ? ", " : "",
                              (unsigned long long)(next - sidecar->scenes[s])) > 0;
        }

        written =
            written && fprintf(sidecar->hdr10plus,
                               "]\n  },\n  \"ToolInfo\": {\n    \"Tool\": \"libhdrgrading\",\n"
                               "    \"Version\": \"%s\"\n  }\n}\n",
                               hdrg_version()) > 0;
        written = (fclose(sidecar->hdr10plus) == 0) && written;
    }

    if (sidecar->st2094_50 != NULL) {
        written = fputs("\n  ]\n}\n", sidecar->st2094_50) != EOF && written;
        written = (fclose(sidecar->st2094_50) == 0) && written;
    }

    free(sidecar->scenes);
    memset(sidecar, 0, sizeof(*sidecar));

    return written;
}

/* the static mapping of params on caps as a .cube file of size nodes an axis */
static int video_export_lut(const char *path, uint8_t primaries, uint8_t transfer,
                            const hdrg_params_t *params, const hdrg_display_caps_t *caps,
                            uint16_t size)
{
    const size_t nodes = (size_t)size * size * size;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    FILE *file;
    size_t k;
    bool written;
    float *rgb;

    rgb = (float *)malloc(nodes * 3u * sizeof(float));
    if (rgb == NULL) {
        fprintf(stderr, "out of memory\n");

        return 1;
    }

    status = hdrg_context_create(NULL, &ctx);
    if (status == HDRG_OK) {
        status = hdrg_lut3d_export(ctx, primaries, transfer, params, caps, size, rgb);
        hdrg_context_destroy(ctx);
    }

    if (status != HDRG_OK) {
        fprintf(stderr, "hdrg_lut3d_export: %s\n", hdrg_status_string(status));
        free(rgb);

        return 1;
    }

    file = fopen(path, "w");
    written =
        file != NULL &&
        fprintf(file,
                "# libhdrgrading static mapping: source R'G'B' codes -> SMPTE ST 2084 "
                "R'G'B' (%s primaries)\nTITLE \"hdrg static\"\nLUT_3D_SIZE %u\n"
                "DOMAIN_MIN 0.0 0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n",
                (caps->gamut == HDRG_GAMUT_BT2020) ? "BT.2020" : "Display P3", (unsigned)size) > 0;

    for (k = 0; written && k < nodes; k++) {
        written = fprintf(file, "%.6f %.6f %.6f\n", (double)rgb[k * 3u], (double)rgb[k * 3u + 1u],
                          (double)rgb[k * 3u + 2u]) > 0;
    }

    if (file != NULL && fclose(file) != 0) {
        written = false;
    }

    free(rgb);

    if (!written) {
        fprintf(stderr, "cannot write %s\n", path);

        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *input_path, *output_path, *stats_path, *diffuse_max, *strength, *lut_path, *denoise,
        *deblock, *dering, *defringe, *deband, *mctf, *chroma_fit, *upscaler, *effort,
        *hdr10plus_path, *st2094_50_path, *sets[VIDEO_MAX_SETS];
    hdrg_context_desc_t desc;
    hdrg_params_t params;
    hdrg_session_format_t format;
    hdrg_session_config_t window;
    hdrg_display_caps_t caps;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_latency_t latency;
    hdrg_session_pass_t *passes;
    hdrg_session_light_t *lights;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    video_y4m_t y4m_in, y4m_out;
    video_input_t source;
    tools_filter_t filters[TOOLS_MAX_FILTERS];
    video_sidecar_t sidecar;
    FILE *input, *output, *stats;
    uint64_t frames, sent, flashes;
    uint32_t width, height, pass_count, lut_size, filter_count, set_count, k;
    uint8_t *rows, p, preset, threads, gpu, gpu_analysis, depth, out_depth, primaries, transfer,
        matrix, chroma_loc;
    int i;
    size_t out_offsets[3], out_strides[3], out_bytes;
    bool dither, ended, bt2020, y4m;
    double start, total;
    float lift, last_lift, step_sum, step_max, max_cll, max_fall, peak;

    input_path = NULL;
    output_path = NULL;
    stats_path = NULL;
    diffuse_max = NULL;
    strength = NULL;
    denoise = NULL;
    deblock = NULL;
    dering = NULL;
    defringe = NULL;
    deband = NULL;
    mctf = NULL;
    chroma_fit = NULL;
    upscaler = NULL;
    effort = NULL;
    hdr10plus_path = NULL;
    st2094_50_path = NULL;
    memset(&source, 0, sizeof(source));
    source.max_frames = UINT64_MAX;
    passes = NULL;
    lights = NULL;
    pass_count = 0;
    filter_count = 0;
    set_count = 0;
    lut_path = NULL;
    lut_size = 33;
    hdrg_session_config_init(&window);
    window.latency = HDRG_LATENCY_OFFLINE;
    memset(&y4m_out, 0, sizeof(y4m_out));
    y4m_out.rate_num = 30;
    y4m_out.rate_den = 1;
    bt2020 = false;
    y4m = false;
    gpu = HDRG_GPU_OFF;
    gpu_analysis = HDRG_GPU_ANALYSIS_EXACT;
    width = 0;
    height = 0;
    preset = HDRG_PRESET_PHOTO;
    threads = 0;
    depth = 8;
    out_depth = 8;
    primaries = HDRG_PRIMARIES_BT709;
    transfer = HDRG_TRANSFER_SRGB;
    matrix = 0;
    chroma_loc = UINT8_MAX;
    dither = true;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &width, &height) != 2) {
                usage();

                return 2;
            }
        } else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            depth = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--primaries") == 0 && i + 1 < argc) {
            primaries = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--transfer") == 0 && i + 1 < argc) {
            transfer = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--matrix") == 0 && i + 1 < argc) {
            matrix = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--chroma-loc") == 0 && i + 1 < argc) {
            chroma_loc = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--out-rate") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%u:%u", &window.out_rate_num, &window.out_rate_den) != 2 ||
                window.out_rate_num == 0 || window.out_rate_den == 0) {
                usage();

                return 2;
            }
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%u:%u", &y4m_out.rate_num, &y4m_out.rate_den) != 2) {
                usage();

                return 2;
            }
        } else if (strcmp(argv[i], "--preset") == 0 && i + 1 < argc) {
            if (!tools_named("--preset", "preset", argv[++i], &preset)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--diffuse-max") == 0 && i + 1 < argc) {
            diffuse_max = argv[++i];

        } else if (strcmp(argv[i], "--strength") == 0 && i + 1 < argc) {
            strength = argv[++i];
        } else if (strcmp(argv[i], "--denoise") == 0 && i + 1 < argc) {
            denoise = argv[++i];
        } else if (strcmp(argv[i], "--deblock") == 0 && i + 1 < argc) {
            deblock = argv[++i];
        } else if (strcmp(argv[i], "--dering") == 0 && i + 1 < argc) {
            dering = argv[++i];
        } else if (strcmp(argv[i], "--defringe") == 0 && i + 1 < argc) {
            defringe = argv[++i];
        } else if (strcmp(argv[i], "--deband") == 0 && i + 1 < argc) {
            deband = argv[++i];
        } else if (strcmp(argv[i], "--mctf") == 0 && i + 1 < argc) {
            mctf = argv[++i];
        } else if (strcmp(argv[i], "--effort") == 0 && i + 1 < argc) {
            effort = argv[++i];
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            if (!tools_filter(filters, &filter_count, argv[++i])) {
                return 2;
            }
        } else if (strcmp(argv[i], "--filter-opt") == 0 && i + 1 < argc) {
            if (!tools_filter_opt(filters, filter_count, argv[++i])) {
                return 2;
            }
        } else if (strcmp(argv[i], "--chroma-fit") == 0 && i + 1 < argc) {
            chroma_fit = argv[++i];
        } else if (strcmp(argv[i], "--upscaler") == 0 && i + 1 < argc) {
            upscaler = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--gpu") == 0 && i + 1 < argc) {
            gpu = (strcmp(argv[++i], "auto") == 0)    ? HDRG_GPU_AUTO
                  : (strcmp(argv[i], "filters") == 0) ? HDRG_GPU_FILTERS
                                                      : HDRG_GPU_OFF;
        } else if (strcmp(argv[i], "--gpu-analysis") == 0 && i + 1 < argc) {
            gpu_analysis =
                (strcmp(argv[++i], "fast") == 0) ? HDRG_GPU_ANALYSIS_FAST : HDRG_GPU_ANALYSIS_EXACT;
        } else if (strcmp(argv[i], "--shed") == 0) {
            window.shed = true;
        } else if (strcmp(argv[i], "--out-depth") == 0 && i + 1 < argc) {
            out_depth = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bt2020") == 0) {
            bt2020 = true;
        } else if (strcmp(argv[i], "--y4m") == 0) {
            y4m = true;
        } else if (strcmp(argv[i], "--no-dither") == 0) {
            dither = false;
        } else if (strcmp(argv[i], "--no-flash-limit") == 0) {
            window.flash_off = true;
        } else if (strcmp(argv[i], "--export-lut") == 0 && i + 1 < argc) {
            lut_path = argv[++i];
        } else if (strcmp(argv[i], "--lut-size") == 0 && i + 1 < argc) {
            lut_size = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--temporal") == 0 && i + 1 < argc) {
            if (!tools_named("--temporal", "temporal", argv[++i], &window.temporal)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--past") == 0 && i + 1 < argc) {
            window.past = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output-size") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%ux%u", &window.output_width, &window.output_height) != 2) {
                usage();

                return 2;
            }
        } else if (strcmp(argv[i], "--proxy") == 0 && i + 1 < argc) {
            window.proxy = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--future") == 0 && i + 1 < argc) {
            window.future = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--latency") == 0 && i + 1 < argc) {
            if (!tools_named("--latency", "latency", argv[++i], &window.latency)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc && set_count < VIDEO_MAX_SETS) {
            sets[set_count++] = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            source.max_frames = (uint64_t)strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            stats_path = argv[++i];
        } else if (strcmp(argv[i], "--hdr10plus-json") == 0 && i + 1 < argc) {
            hdr10plus_path = argv[++i];
        } else if (strcmp(argv[i], "--st2094-50-json") == 0 && i + 1 < argc) {
            st2094_50_path = argv[++i];
        } else if (input_path == NULL) {
            input_path = argv[i];
        } else if (output_path == NULL) {
            output_path = argv[i];
        } else {
            usage();

            return 2;
        }
    }

    if ((lut_path == NULL && (input_path == NULL || output_path == NULL)) ||
        (depth != 8 && depth != 16) || (out_depth != 8 && out_depth != 16) ||
        y4m_out.rate_num == 0 || y4m_out.rate_den == 0 || lut_size < 2u ||
        lut_size > HDRG_LUT3D_MAX_SIZE) {
        usage();

        return 2;
    }

    hdrg_params_init(&params, preset);
    window.dither_off = !dither;

    if (diffuse_max != NULL) {
        params.diffuse_max = (float)atof(diffuse_max);
    }

    if (strength != NULL) {
        params.global_strength = (float)atof(strength);
    }

    if (denoise != NULL) {
        params.grain_policy = HDRG_GRAIN_REDUCE;
        params.denoise_strength = (float)atof(denoise);
    }

    if (deblock != NULL) {
        params.deblock_strength = (float)atof(deblock);
    }

    if (dering != NULL) {
        params.dering_strength = (float)atof(dering);
    }

    if (defringe != NULL) {
        params.defringe_strength = (float)atof(defringe);
    }

    if (deband != NULL) {
        params.deband_strength = (float)atof(deband);
    }

    if (mctf != NULL) {
        params.mctf_strength = (float)atof(mctf);
    }

    if ((effort != NULL && !tools_named("--effort", "effort", effort, &params.effort)) ||
        (chroma_fit != NULL &&
         !tools_named("--chroma-fit", "chroma-fit", chroma_fit, &params.chroma_fit)) ||
        (upscaler != NULL && !tools_named("--upscaler", "upscaler", upscaler, &params.upscaler))) {
        return 2;
    }

    hdrg_display_caps_init(&caps);
    caps.gamut = (bt2020 || y4m) ? HDRG_GAMUT_BT2020 : HDRG_GAMUT_P3;

    for (k = 0; k < set_count; k++) {
        if (!tools_set(sets[k], &params, &caps, NULL, &window)) {
            return 2;
        }
    }

    if (lut_path != NULL) {
        return video_export_lut(lut_path, primaries, transfer, &params, &caps, (uint16_t)lut_size);
    }

    if (matrix == 0) {
        matrix = (primaries == HDRG_PRIMARIES_BT2020)   ? HDRG_MATRIX_BT2020
                 : (primaries == 5u || primaries == 6u) ? HDRG_MATRIX_BT601
                                                        : HDRG_MATRIX_BT709;
    }

    /* BT.470 BG and SMPTE 170M share the matrix */
    matrix = (matrix == 5u) ? HDRG_MATRIX_BT601 : matrix;

    input = video_open(input_path, "rb", stdin);
    output = video_open(output_path, "wb", stdout);
    stats = (stats_path != NULL) ? fopen(stats_path, "w") : NULL;
    if (input == NULL || output == NULL || (stats_path != NULL && stats == NULL)) {
        fprintf(stderr, "cannot open the input, the output or the statistics\n");

        return 1;
    }

    if (window.latency == HDRG_LATENCY_LOW && window.future > 0 &&
        window.future != HDRG_SESSION_AUTO) {
        fprintf(stderr, "--latency low waits for no frames after a frame: no --future\n");

        return 2;
    }

    if (window.temporal == HDRG_TEMPORAL_TWO_PASS && input == stdin) {
        fprintf(stderr, "--temporal two-pass reads the input twice: give a file, not -\n");

        return 2;
    }

    memset(&format, 0, sizeof(format));
    format.primaries = primaries;
    format.transfer = transfer;

    /* a y4m stream's frames, or raw RGB of --size */
    if (width == 0) {
        if (!video_y4m_header(input, &y4m_in)) {
            fprintf(stderr, "the input is not a y4m stream of 4:2:0 / 4:2:2 / 4:4:4 (raw RGB "
                            "needs --size)\n");

            return 1;
        }

        width = y4m_in.width;
        height = y4m_in.height;
        y4m_out.rate_num = y4m_in.rate_num;
        y4m_out.rate_den = y4m_in.rate_den;
        window.input.layout = y4m_in.layout;
        window.input.matrix = matrix;
        window.input.bits = y4m_in.bits;
        window.input.full_range = y4m_in.full_range;
        window.input.chroma_loc = (chroma_loc != UINT8_MAX) ? chroma_loc : y4m_in.chroma_loc;
        source.bytes = video_y4m_layout(&y4m_in, source.offsets, source.strides);
    } else {
        format.bit_depth = depth;
        format.stride = (size_t)width * 3u * (depth / 8u);
        source.bytes = format.stride * height;
    }

    source.file = input;
    source.layout = window.input.layout;
    window.rate_num = y4m_out.rate_num;
    window.rate_den = y4m_out.rate_den;

    format.width = width;
    format.height = height;

    if (y4m) {
        window.output.layout = HDRG_YUV_420;
        window.output.matrix = HDRG_MATRIX_BT2020;
        window.output.bits = 10;
    } else {
        window.output.bits = out_depth;
    }

    /* a frame is at most 6 bytes a pixel: it must fit size_t (32-bit targets) */
    if (width > UINT16_MAX || height > UINT16_MAX || (uint64_t)width * height > SIZE_MAX / 6u) {
        fprintf(stderr, "the frames are too large\n");

        return 1;
    }

    hdrg_context_desc_init(&desc);
    desc.thread_count = threads;
    desc.gpu = gpu;
    desc.gpu_analysis = gpu_analysis;
    status = hdrg_context_create(&desc, &ctx);

    if (status == HDRG_OK &&
        !tools_add_filters(ctx, filters, filter_count,
                           HDRG_STAGE_SCALE | HDRG_STAGE_PREFILTER | HDRG_STAGE_INTERP)) {
        return 2;
    }

    if (status == HDRG_OK && window.temporal == HDRG_TEMPORAL_TWO_PASS) {
        status = video_first_pass(ctx, &format, &params, &caps, &window, &source, &passes, &lights,
                                  &pass_count);
        window.passes = passes;
        window.pass_count = pass_count;
    }

    if (status == HDRG_OK) {
        status = hdrg_session_create(ctx, &format, &params, &caps, &window, &session);
    }

    if (status != HDRG_OK) {
        fprintf(stderr, "hdrg_session_create: %s\n", hdrg_status_string(status));

        return 1;
    }

    /* the output at the session's size (the SCALE stage's) */
    hdrg_session_size(session, &y4m_out.width, &y4m_out.height);

    if (y4m) {
        y4m_out.layout = HDRG_YUV_420;
        y4m_out.bits = 10;
        out_bytes = video_y4m_layout(&y4m_out, out_offsets, out_strides);
    } else {
        out_strides[0] = (size_t)y4m_out.width * 3u * (out_depth / 8u);
        out_bytes = out_strides[0] * y4m_out.height;
    }

    rows = (uint8_t *)malloc(out_bytes);
    if (rows == NULL) {
        fprintf(stderr, "out of memory\n");

        return 1;
    }

    if (stats != NULL) {
        fprintf(stats, "frame,cut,sdr_apl_nits,apl_nits,lift_stops,apl_scale,apl_fit,peak_nits,"
                       "anchor,anchor_used,key,flash,fade,noise,blockiness,ringing\n");
    }

    if (y4m) {
        fprintf(output,
                "YUV4MPEG2 W%u H%u F%u:%u Ip A1:1 C420p10 XYSCSS=420P10 XCOLORRANGE=LIMITED\n",
                y4m_out.width, y4m_out.height,
                (window.out_rate_num != 0) ? window.out_rate_num : y4m_out.rate_num,
                (window.out_rate_num != 0) ? window.out_rate_den : y4m_out.rate_den);
    }

    if (!video_sidecar_open(&sidecar, hdr10plus_path, st2094_50_path)) {
        fprintf(stderr, "cannot write the sidecars\n");

        return 1;
    }

    frames = 0;
    sent = 0;
    flashes = 0;
    ended = false;
    last_lift = 0.0f;
    step_sum = 0.0f;
    step_max = 0.0f;
    max_cll = 0.0f;
    max_fall = 0.0f;
    peak = 0.0f;
    start = video_now();

    /* receive while frames come out; send the next one (or the end) when the session asks */
    for (;;) {
        memset(&out, 0, sizeof(out));
        for (p = 0; p < 3u; p++) {
            out.planes[p] = y4m ? rows + out_offsets[p] : (p == 0) ? rows : NULL;
            out.strides[p] = (y4m || p == 0) ? out_strides[p] : 0;
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN && !ended) {
            if (video_next(&source, sent, &in)) {
                status = hdrg_session_send_frame(session, &in);
                sent++;
            } else if (source.failed) {
                status = HDRG_ERR_NOMEM;
            } else {
                status = hdrg_session_send_frame(session, NULL);
                ended = true;
            }

            if (status == HDRG_OK) {
                continue;
            }
        }

        if (status == HDRG_EOF) {
            status = HDRG_OK;

            break;
        }

        if (status != HDRG_OK) {
            fprintf(stderr, "frame %llu: %s\n", (unsigned long long)frames,
                    hdrg_status_string(status));

            break;
        }

        if ((y4m && fputs("FRAME\n", output) == EOF) ||
            fwrite(rows, 1, out_bytes, output) != out_bytes) {
            fprintf(stderr, "cannot write the output\n");
            status = HDRG_ERR_INTERNAL;

            break;
        }

        if (!video_sidecar_frame(&sidecar, &out, frames)) {
            fprintf(stderr, "cannot write the sidecars\n");
            status = HDRG_ERR_INTERNAL;

            break;
        }

        /* static renders without statistics: no estimated lift */
        lift = (out.sdr_apl_nits > 0.0f) ? log2f(out.apl_nits / out.sdr_apl_nits) : 0.0f;

        if (frames > 0 && !out.scene_cut) {
            step_sum += fabsf(lift - last_lift);
            step_max = (fabsf(lift - last_lift) > step_max) ? fabsf(lift - last_lift) : step_max;
        }

        if (stats != NULL) {
            fprintf(stats,
                    "%llu,%d,%.3f,%.3f,%.5f,%.5f,%.5f,%.1f,%.5f,%.5f,%.5f,%d,%d,%.3f,%.3f,%.3f\n",
                    (unsigned long long)frames, out.scene_cut ? 1 : 0, (double)out.sdr_apl_nits,
                    (double)out.apl_nits, (double)lift, (double)out.apl_scale, (double)out.apl_fit,
                    (double)out.peak_nits, (double)out.anchor_luma, (double)out.anchor_used,
                    (double)out.key_luma, out.flash_limited ? 1 : 0, out.fading ? 1 : 0,
                    (double)out.noise_sigma, (double)out.blockiness, (double)out.ringing);
        }

        flashes += out.flash_limited ? 1u : 0u;
        max_cll = (out.max_nits > max_cll) ? out.max_nits : max_cll;
        max_fall = (out.avg_nits > max_fall) ? out.avg_nits : max_fall;
        peak = (out.peak_nits > peak) ? out.peak_nits : peak;
        last_lift = lift;
        frames++;
    }

    total = video_now() - start;

    if (!video_sidecar_close(&sidecar, frames) && status == HDRG_OK) {
        fprintf(stderr, "cannot write the sidecars\n");
        status = HDRG_ERR_INTERNAL;
    }

    latency.struct_size = (uint32_t)sizeof(latency);
    (void)hdrg_session_latency_report(session, &latency);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    if (frames > 0) {
        fprintf(stderr,
                "%llu frames, %.1f ms a frame; lift change a frame (within scenes): mean %.4f, "
                "max %.4f stops; %llu flash-limited\n",
                (unsigned long long)frames, total * 1e3 / (double)frames,
                (frames > 1) ? (double)(step_sum / (float)(frames - 1u)) : 0.0, (double)step_max,
                (unsigned long long)flashes);
        /* x265: the mastering display is Display P3 (D65) up to the rendition peak */
        fprintf(stderr,
                "--master-display \"G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)"
                "L(%.0f,50)\" --max-cll \"%.0f,%.0f\"%s\n",
                (double)peak * 10000.0, (double)max_cll, (double)max_fall,
                y4m ? " --colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc "
                      "--range limited --output-depth 10"
                    : "");
        fprintf(stderr,
                "latency (%s): %u frames (window %u, prefilter %u + %u in flight, scale %u + %u "
                "in flight); %.1f ms at %.3f fps + work %.1f ms a frame (longest call "
                "%.1f ms) = %.1f ms; playback %u / low %u frames; preroll %u\n",
                video_profile_name(latency.profile), latency.frames, latency.window,
                latency.prefilter, latency.prefilter_flight, latency.scale, latency.scale_flight,
                (double)latency.frames_ms, (double)latency.rate, (double)latency.work_ms,
                (double)latency.work_max_ms, (double)latency.total_ms,
                latency.profiles[HDRG_LATENCY_PLAYBACK], latency.profiles[HDRG_LATENCY_LOW],
                latency.preroll);
        fprintf(stderr,
                "work a frame: convert %.1f ms%s, prefilter %.1f, analyze %.1f, interp %.1f, "
                "apply %.1f ms%s (interval %.1f ms)\n",
                (double)latency.convert_ms, video_device_name(latency.convert_device),
                (double)latency.prefilter_ms, (double)latency.analyze_ms, (double)latency.interp_ms,
                (double)latency.apply_ms, video_device_name(latency.apply_device),
                (double)latency.interval_ms);

        if (latency.frames_on_gpu) {
            fprintf(stderr, "the frames stay on the GPU (proxies and noise windows read back)\n");
        }

        if (latency.late_frames > 0) {
            video_advice(&latency);
        }

        if (latency.shed_at != UINT32_MAX) {
            fprintf(stderr, "the prefilters shed from frame %u on (--shed)\n", latency.shed_at);
        }
    }

    video_input_free(&source);
    free(rows);
    free(passes);
    free(lights);

    if (stats != NULL) {
        fclose(stats);
    }

    if (input != stdin) {
        fclose(input);
    }

    if (output != stdout) {
        fclose(output);
    }

    return (status == HDRG_OK) ? 0 : 1;
}
