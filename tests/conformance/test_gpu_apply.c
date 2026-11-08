/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "core/gpu_apply.h"
#include "core/grain.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"
#include "priv.h"

/*
 * The apply pass on the GPU (the context's HDRG_GPU_AUTO, src/core/gpu_apply)
 * against the CPU's (HDRG_GPU_FILTERS: the same filters, the CPU's apply):
 * every output sample within 1 code and few of them off, the light within
 * 1e-3 (HDR10+'s percentiles within 3%, ST 2094-50's gains 0.05 stops), over RGB / RGBA / 16-bit /
 * Y'CbCr input, 16-bit and dithered 8-bit R'G'B' and Y'CbCr 4:2:0 output, odd sizes, a noisy source
 * (the coring), a fixed plan (STATIC), a contrast and a clarity (its base) and the presets (PHOTO's
 * resynthesized grain on the noisy source: over the R'G'B' inputs, the Y'CbCr ones' conversions
 * telling its measure apart; a codec's film grain, HDRG_SD_FILM_GRAIN, over both); the GPU's output
 * the same twice. The Y'CbCr input is converted on the same GPU (src/core/gpu_convert), and its
 * frames stay on the GPU to the apply (the PREFILTER chain on the device, the analysis on the
 * proxies and noise windows read back): the same output, bit for bit, as with the frames read back
 * to the host (the grain too). The state a session gives the app's GPU (hdrg_session_receive_state,
 * the library without a GPU), run with the WGSL as an app would, gives the GPU apply's output bit
 * for bit (a codec's film grain in its words too). Ignored without a GPU (the report's apply_device
 * 0).
 */

#define TGA_FRAMES    4u
#define TGA_IN_RGB8   0u
#define TGA_IN_RGBA8  1u
#define TGA_IN_RGB16  2u
#define TGA_IN_YUV    3u
#define TGA_OUT_RGB16 0u
#define TGA_OUT_RGB8  1u
#define TGA_OUT_YUV   2u
#define TGA_NOISE_KEY UINT64_C(0x5447415F4E4F4953)

typedef struct {
    uint32_t width, height;
    uint8_t preset, input, output, temporal;
    bool noisy;
    bool graded; /* a contrast of 1.2 and a clarity of 0.3 (the clarity base) */
} tga_case_t;

/* a case's run: its output frames, their light, the apply's device, the conversion's */
typedef struct {
    uint8_t *frames;
    hdrg_hdr10plus_t plus[TGA_FRAMES];  /* the outputs' HDR10+ ... */
    hdrg_st2094_50_t curve[TGA_FRAMES]; /* ... and ST 2094-50 */
    float nits[TGA_FRAMES][2];
    uint32_t device, convert;
    bool resident; /* the frames stayed on the GPU */
} tga_run_t;

#define TGA_GV_YUV    6u /* src/core/gpu_convert.c's slots: the planes, the sites, the knots ... */
#define TGA_GV_BASE   7u
#define TGA_GV_KNOTS  8u
#define TGA_GV_VALUES 9u

static uint8_t g_frames;      /* tga_session: where the context's sessions keep frames */
static bool g_mctf;           /* tga_session: the preset's hdrg.mctf (its GPU version's windows) */
static bool g_preserve;       /* tga_session: the grain kept as it is (not the preset's RESYNTH) */
static const uint8_t *g_film; /* tga_session: the frames' HDRG_SD_FILM_GRAIN (NULL: none) */
static bool g_packed;         /* tga_session: Y'CbCr in as NV12, out as P010 (Cb, Cr alternating) */
static uint8_t g_yuv_bits = 10; /* tga_session: the Y'CbCr output's depth (8: a byte a sample) */
static uint8_t g_analysis;      /* tga_session: the context's HDRG_GPU_ANALYSIS_* */
static uint32_t g_kind;         /* the GPU of the run's contexts (HDRG_DEVICE_*; 0: the first) */

static size_t tga_out_bytes(const tga_case_t *c)
{
    const size_t pixels = (size_t)c->width * c->height,
                 chroma = (size_t)((c->width + 1u) / 2u) * ((c->height + 1u) / 2u);

    if (c->output == TGA_OUT_YUV) {
        return (pixels + 2u * chroma) * ((g_yuv_bits > 8u) ? 2u : 1u);
    }

    return pixels * 3u * ((c->output == TGA_OUT_RGB16) ? 2u : 1u);
}

/* the 8-bit R'G'B' of frame t: the test image panned by t, noise of +-6 codes when noisy */
static void tga_rgb(const tga_case_t *c, const uint8_t *image, uint32_t t, uint8_t *rgb)
{
    uint32_t x, y, h;
    uint8_t ch;
    int32_t v;

    for (y = 0; y < c->height; y++) {
        for (x = 0; x < c->width; x++) {
            for (ch = 0; ch < 3u; ch++) {
                v = image[((size_t)y * c->width + (x + t) % c->width) * 3u + ch];

                if (c->noisy) {
                    h = hdrg_hash_u32(TGA_NOISE_KEY, x, y, t * 3u + ch);
                    v += (int32_t)(h % 13u) - 6;
                }

                rgb[((size_t)y * c->width + x) * 3u + ch] =
                    (uint8_t)HDRG_TEST_MAX(0, (v > 255) ? 255 : v);
            }
        }
    }
}

/* frame t in the case's input form into data (its planes and strides set in frame) */
static void tga_frame(const tga_case_t *c, const uint8_t *rgb, uint8_t *data,
                      hdrg_session_frame_t *frame)
{
    const uint32_t cw = (c->width + 1u) / 2u, chh = (c->height + 1u) / 2u;
    const uint8_t *p;
    uint32_t x, y, i;
    uint16_t wide;
    uint8_t *cb, *cr, ch;
    size_t step;
    float r, g, b, sum_b, sum_r, luma;

    memset(frame, 0, sizeof(*frame));
    frame->planes[0] = data;

    if (c->input == TGA_IN_RGB8) {
        memcpy(data, rgb, (size_t)c->width * c->height * 3u);

        return;
    }

    if (c->input == TGA_IN_RGBA8 || c->input == TGA_IN_RGB16) {
        for (i = 0; i < c->width * c->height; i++) {
            for (ch = 0; ch < 3u; ch++) {
                if (c->input == TGA_IN_RGBA8) {
                    data[(size_t)i * 4u + ch] = rgb[(size_t)i * 3u + ch];
                } else {
                    wide = (uint16_t)(rgb[(size_t)i * 3u + ch] * 257u + (i * 7u + ch) % 200u);
                    memcpy(data + ((size_t)i * 3u + ch) * 2u, &wide, sizeof(wide));
                }
            }

            if (c->input == TGA_IN_RGBA8) {
                data[(size_t)i * 4u + 3u] = 255u;
            }
        }

        return;
    }

    /* BT.709 limited range 8-bit 4:2:0: the luma per pixel, the chroma of each 2 x 2 */
    cb = data + (size_t)c->width * c->height;
    cr = cb + (size_t)cw * chh;
    frame->planes[1] = cb;
    frame->planes[2] = cr;
    frame->strides[0] = c->width;
    frame->strides[1] = cw;
    frame->strides[2] = cw;
    step = 1;

    /* NV12: Cb, Cr alternating */
    if (g_packed) {
        cr = cb + 1;
        frame->planes[2] = NULL;
        frame->strides[1] = 2u * (size_t)cw;
        frame->strides[2] = 0;
        step = 2;
    }

    for (y = 0; y < c->height; y++) {
        for (x = 0; x < c->width; x++) {
            p = rgb + ((size_t)y * c->width + x) * 3u;
            luma = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
            data[(size_t)y * c->width + x] = (uint8_t)(16.0f + 219.0f * luma + 0.5f);
        }
    }

    for (y = 0; y < chh; y++) {
        for (x = 0; x < cw; x++) {
            p = rgb + ((size_t)(2u * y) * c->width + 2u * x) * 3u;
            r = (float)p[0] / 255.0f;
            g = (float)p[1] / 255.0f;
            b = (float)p[2] / 255.0f;
            luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            sum_b = (b - luma) / 1.8556f;
            sum_r = (r - luma) / 1.5748f;
            cb[((size_t)y * cw + x) * step] = (uint8_t)(128.0f + 224.0f * sum_b + 0.5f);
            cr[((size_t)y * cw + x) * step] = (uint8_t)(128.0f + 224.0f * sum_r + 0.5f);
        }
    }
}

/* the case's frames through a session on a context of gpu */
static void tga_session(const tga_case_t *c, uint8_t gpu, tga_run_t *run)
{
    const size_t frame_bytes = tga_out_bytes(c), pixels = (size_t)c->width * c->height,
                 chroma = (size_t)((c->width + 1u) / 2u) * ((c->height + 1u) / 2u),
                 sample = (g_yuv_bits > 8u) ? 2u : 1u;
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_latency_t report;
    hdrg_status_t status;
    uint32_t sent, received;
    uint8_t *image, *rgb, *data, *dst;

    image = hdrg_test_image(c->width, c->height);
    rgb = (uint8_t *)malloc(pixels * 3u);
    data = (uint8_t *)malloc(pixels * 6u * TGA_FRAMES);
    TEST_ASSERT_TRUE(image != NULL && rgb != NULL && data != NULL);

    hdrg_context_desc_init(&desc);
    desc.gpu = gpu;
    desc.gpu_analysis = g_analysis;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    ctx->gpu_frames = g_frames;
    ctx->gpu_kind = g_kind;

    memset(&format, 0, sizeof(format));
    format.width = c->width;
    format.height = c->height;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = (c->input == TGA_IN_RGB16) ? 16 : 8;
    format.has_alpha = c->input == TGA_IN_RGBA8;
    format.stride = (size_t)c->width * (format.has_alpha ? 4u : 3u) * (format.bit_depth / 8u);

    hdrg_params_init(&params, c->preset);

    params.mctf_strength = g_mctf ? params.mctf_strength : 0.0f;
    params.grain_policy = g_preserve ? HDRG_GRAIN_PRESERVE : params.grain_policy;
    params.contrast = c->graded ? 1.2f : params.contrast;
    params.clarity = c->graded ? 0.3f : params.clarity;
    hdrg_display_caps_init(&caps);
    hdrg_session_config_init(&config);
    config.temporal = c->temporal;
    config.past = (c->temporal == HDRG_TEMPORAL_STATIC) ? 0u : config.past;
    config.output.bits = (c->output == TGA_OUT_RGB16) ? 16u : 8u;

    if (c->input == TGA_IN_YUV) {
        config.input.layout = HDRG_YUV_420;
        config.input.matrix = HDRG_MATRIX_BT709;
        config.input.bits = 8;
        config.input.interleaved = g_packed;
    }

    if (c->output == TGA_OUT_YUV) {
        config.output.layout = HDRG_YUV_420;
        config.output.matrix = HDRG_MATRIX_BT2020;
        config.output.bits = g_yuv_bits;
        config.output.interleaved = g_packed;
        config.output.msb = g_packed && g_yuv_bits > 8u;
        caps.gamut = HDRG_GAMUT_BT2020;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, &caps, &config, &session));

    sent = 0;
    received = 0;
    for (;;) {
        dst = run->frames + (size_t)received * frame_bytes;
        memset(&out, 0, sizeof(out));
        out.planes[0] = dst;
        out.strides[0] = (size_t)c->width * 3u * ((c->output == TGA_OUT_RGB16) ? 2u : 1u);

        if (c->output == TGA_OUT_YUV) {
            out.planes[1] = dst + pixels * sample;
            out.planes[2] = g_packed ? NULL : dst + (pixels + chroma) * sample;
            out.strides[0] = (size_t)c->width * sample;
            out.strides[1] = (size_t)((c->width + 1u) / 2u) * sample * (g_packed ? 2u : 1u);
            out.strides[2] = g_packed ? 0u : out.strides[1];
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            if (sent < TGA_FRAMES) {
                tga_rgb(c, image, sent, rgb);
                tga_frame(c, rgb, data + (pixels * 6u * sent), &in);
                in.pts = sent;
                in.film_grain = g_film;
                in.film_grain_size = (g_film != NULL) ? HDRG_GRAIN_FILM_BYTES : 0u;
            }

            /* each frame its own data: an R'G'B' frame is read until its receive */
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TGA_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        run->nits[received][0] = out.max_nits;
        run->nits[received][1] = out.avg_nits;
        run->plus[received] = out.hdr10plus;
        run->curve[received] = out.st2094_50;
        received++;
    }

    TEST_ASSERT_EQUAL_UINT32(TGA_FRAMES, received);

    memset(&report, 0, sizeof(report));
    report.struct_size = (uint32_t)sizeof(report);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_latency_report(session, &report));
    run->device = report.apply_device;
    run->convert = report.convert_device;
    run->resident = report.frames_on_gpu;

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(image);
    free(rgb);
    free(data);
}

/*
 * hdrg.mctf's windows on the GPU's frames (the copies of the chain's frames there) as the host's
 * frames give them (the GPU's conversion, within a code of the CPU's, is not compared with
 * the CPU's: the motion and the weights may tell its codes apart); a noisy source changed
 * from without (plain)
 */
static void tga_resident_mctf(const tga_case_t *c, size_t bytes, const uint8_t *plain)
{
    tga_run_t kept, host;

    kept.frames = (uint8_t *)malloc(bytes);
    host.frames = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(kept.frames != NULL && host.frames != NULL);

    g_mctf = true;
    g_frames = HDRG_GPU_FRAMES_GPU;
    tga_session(c, HDRG_GPU_AUTO, &kept);
    g_frames = HDRG_GPU_FRAMES_HOST;
    tga_session(c, HDRG_GPU_AUTO, &host);
    g_frames = HDRG_GPU_FRAMES_CHOSEN;
    g_mctf = false;
    TEST_ASSERT_TRUE(kept.resident);
    TEST_ASSERT_FALSE(host.resident);
    TEST_ASSERT_EQUAL_MEMORY(host.frames, kept.frames, bytes);
    TEST_ASSERT_EQUAL_MEMORY(host.nits, kept.nits, sizeof(host.nits));
    TEST_ASSERT_EQUAL_MEMORY(host.plus, kept.plus, sizeof(host.plus));
    TEST_ASSERT_EQUAL_MEMORY(host.curve, kept.curve, sizeof(host.curve));
    TEST_ASSERT_TRUE(!c->noisy || memcmp(plain, kept.frames, bytes) != 0);

    free(kept.frames);
    free(host.frames);
}

/*
 * the GPU's HDR10+ against the CPU's: R, G, B's largest within 1e-3, the
 * percentiles within 3% (a code apart: a bin of 1/8 octave apart at most)
 */
static void tga_plus_near(const hdrg_hdr10plus_t *cpu, const hdrg_hdr10plus_t *gpu)
{
    uint8_t i;

    TEST_ASSERT_TRUE(cpu->valid);
    TEST_ASSERT_TRUE(gpu->valid);

    for (i = 0; i < 3u; i++) {
        HDRG_TEST_NEAR(1e-3 * cpu->maxscl[i] + 1e-3, cpu->maxscl[i], gpu->maxscl[i]);
    }

    for (i = 0; i < HDRG_HDR10PLUS_PERCENTILES; i++) {
        HDRG_TEST_NEAR(0.03 * cpu->percentiles[i] + 1e-3, cpu->percentiles[i], gpu->percentiles[i]);
    }
}

/*
 * the GPU's ST 2094-50 against the CPU's: the curve each gives back at the
 * CPU's points within 0.05 stops (a code apart: a sample in another bin)
 */
static void tga_curve_near(const hdrg_st2094_50_t *cpu, const hdrg_st2094_50_t *gpu)
{
    uint8_t i, j;
    float best;

    TEST_ASSERT_EQUAL(cpu->valid, gpu->valid);

    for (i = 0; cpu->valid && i < cpu->count; i++) {
        best = gpu->y[0];

        for (j = 0; j < gpu->count; j++) {
            best = (fabsf(gpu->x[j] - cpu->x[i]) < 0.03f * cpu->x[i]) ? gpu->y[j] : best;
        }

        HDRG_TEST_NEAR(0.05, cpu->y[i], best);
    }
}

/* the GPU's run of a case against the CPU's; false: no GPU */
static bool tga_compare(const tga_case_t *c)
{
    const size_t bytes = tga_out_bytes(c) * TGA_FRAMES;
    const bool wide = c->output == TGA_OUT_RGB16 || (c->output == TGA_OUT_YUV && g_yuv_bits > 8u);
    tga_run_t cpu, gpu, again, kept;
    uint32_t a, b, d, worst;
    uint8_t f;
    size_t i, n, off;

    cpu.frames = (uint8_t *)malloc(bytes);
    gpu.frames = (uint8_t *)malloc(bytes);
    again.frames = (uint8_t *)malloc(bytes);
    kept.frames = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(cpu.frames != NULL && gpu.frames != NULL && again.frames != NULL &&
                     kept.frames != NULL);

    /*
     * Y'CbCr: the conversions a code apart tell the measure of the grain the
     * chain removed apart, its level all over the frame: the apply's compared
     * with the grain kept (RESYNTH's compared over the other inputs)
     */
    g_preserve = c->input == TGA_IN_YUV;
    tga_session(c, HDRG_GPU_FILTERS, &cpu);
    g_frames = HDRG_GPU_FRAMES_GPU;
    tga_session(c, HDRG_GPU_AUTO, &gpu);
    g_frames = HDRG_GPU_FRAMES_CHOSEN;
    TEST_ASSERT_EQUAL_UINT32(0, cpu.device);
    TEST_ASSERT_EQUAL_UINT32(0, cpu.convert);

    /* the Y'CbCr input converted on the same GPU */
    TEST_ASSERT_EQUAL_UINT32((c->input == TGA_IN_YUV) ? gpu.device : 0u, gpu.convert);

    if (gpu.device == 0u) {
        g_preserve = false;
        free(cpu.frames);
        free(gpu.frames);
        free(again.frames);
        free(kept.frames);

        return false;
    }

    g_frames = HDRG_GPU_FRAMES_GPU;
    tga_session(c, HDRG_GPU_AUTO, &again);
    g_frames = HDRG_GPU_FRAMES_CHOSEN;
    g_preserve = false;
    TEST_ASSERT_EQUAL_MEMORY(gpu.frames, again.frames, bytes);

    /* Y'CbCr: on the GPU from the conversion to the apply, as the host's frames would give */
    TEST_ASSERT_EQUAL(c->input == TGA_IN_YUV, gpu.resident);

    if (gpu.resident) {
        g_frames = HDRG_GPU_FRAMES_GPU;
        tga_session(c, HDRG_GPU_AUTO, &kept);
        g_frames = HDRG_GPU_FRAMES_HOST;
        tga_session(c, HDRG_GPU_AUTO, &again);
        g_frames = HDRG_GPU_FRAMES_CHOSEN;
        TEST_ASSERT_TRUE(kept.resident);
        TEST_ASSERT_FALSE(again.resident);
        TEST_ASSERT_EQUAL_MEMORY(kept.frames, again.frames, bytes);
        TEST_ASSERT_EQUAL_MEMORY(kept.nits, again.nits, sizeof(kept.nits));
        TEST_ASSERT_EQUAL_MEMORY(kept.plus, again.plus, sizeof(kept.plus));
        TEST_ASSERT_EQUAL_MEMORY(kept.curve, again.curve, sizeof(kept.curve));

        if (HDRG_BUILTIN_MCTF != 0) {
            tga_resident_mctf(c, bytes, kept.frames);
        }
    }

    n = wide ? bytes / 2u : bytes;
    off = 0;
    worst = 0;

    for (i = 0; i < n; i++) {
        if (wide) {
            a = (uint32_t)cpu.frames[2u * i] | ((uint32_t)cpu.frames[2u * i + 1u] << 8);
            b = (uint32_t)gpu.frames[2u * i] | ((uint32_t)gpu.frames[2u * i + 1u] << 8);
        } else {
            a = cpu.frames[i];
            b = gpu.frames[i];
        }

        d = (a > b) ? a - b : b - a;
        worst = HDRG_TEST_MAX(worst, d);
        off += (d != 0u) ? 1u : 0u;
    }

    TEST_ASSERT_TRUE(worst <= 1u);
    TEST_ASSERT_TRUE(off * 100u <= n);

    for (f = 0; f < TGA_FRAMES; f++) {
        HDRG_TEST_NEAR(1e-3 * cpu.nits[f][0] + 1e-3, cpu.nits[f][0], gpu.nits[f][0]);
        HDRG_TEST_NEAR(1e-3 * cpu.nits[f][1] + 1e-3, cpu.nits[f][1], gpu.nits[f][1]);
        tga_plus_near(&cpu.plus[f], &gpu.plus[f]);
        tga_curve_near(&cpu.curve[f], &gpu.curve[f]);
    }

    free(cpu.frames);
    free(gpu.frames);
    free(again.frames);
    free(kept.frames);

    return true;
}

static void tga_cases(const tga_case_t *cases, uint32_t count)
{
    uint32_t i, ran;

    ran = 0;

    for (i = 0; i < count; i++) {
        ran += tga_compare(&cases[i]) ? 1u : 0u;
    }

    if (ran == 0u) {
        TEST_IGNORE_MESSAGE("no GPU for the apply pass");
    }
}

/* interleaved R'G'B' (and RGBA, 16-bit) input, 16-bit and dithered 8-bit output, odd sizes */
static void test_rgb(void)
{
    static const tga_case_t cases[] = {
        {160, 96, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, false,
         false},
        {97, 61, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB8, HDRG_TEMPORAL_WINDOWED, false,
         false},
        {66, 45, HDRG_PRESET_GAME, TGA_IN_RGBA8, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, false,
         false},
        {83, 50, HDRG_PRESET_PHOTO, TGA_IN_RGB16, TGA_OUT_RGB8, HDRG_TEMPORAL_WINDOWED, false,
         false},
    };

    tga_cases(cases, (uint32_t)(sizeof(cases) / sizeof(cases[0])));
}

/* Y'CbCr 4:2:0 in and out (10-bit, the BT.2020 matrix), odd sizes, the anime preset's filters */
static void test_yuv(void)
{
    static const tga_case_t cases[] = {
        {128, 72, HDRG_PRESET_ANIME, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
        {99, 67, HDRG_PRESET_PHOTO, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
        {64, 40, HDRG_PRESET_PHOTO, TGA_IN_YUV, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, false,
         false},
        /* analyzed on proxies: the proxy and the noise windows read back */
        {1280, 720, HDRG_PRESET_ANIME, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, true,
         false},
        {1921, 1081, HDRG_PRESET_PHOTO, TGA_IN_YUV, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, true,
         false},
    };

    tga_cases(cases, (uint32_t)(sizeof(cases) / sizeof(cases[0])));
}

/* Y'CbCr 4:2:0 out at 8 bits (a byte a sample, dithered), planar and interleaved (NV12) */
static void test_yuv8(void)
{
    static const tga_case_t cases[] = {
        {99, 67, HDRG_PRESET_PHOTO, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
        {80, 48, HDRG_PRESET_GAME, TGA_IN_RGB8, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
    };

    g_yuv_bits = 8;
    tga_cases(cases, (uint32_t)(sizeof(cases) / sizeof(cases[0])));
    g_packed = true;
    tga_cases(cases, 1);
    g_packed = false;
    g_yuv_bits = 10;
}

/* a noisy source (the coring of La toward its 3 x 3 mean), a fixed plan (STATIC), the clarity */
static void test_cored_and_fixed(void)
{
    static const tga_case_t cases[] = {
        {97, 61, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, true,
         false},
        {90, 54, HDRG_PRESET_ANIME, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, true, false},
        {80, 48, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB16, HDRG_TEMPORAL_STATIC, false, false},
        {81, 47, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB8, HDRG_TEMPORAL_STATIC, true, false},
        {97, 61, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, true, true},
        {72, 40, HDRG_PRESET_ANIME, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, true},
    };

    tga_cases(cases, (uint32_t)(sizeof(cases) / sizeof(cases[0])));
}

/* AV1's film grain: the luma's and (by its function) the chroma's, correlated */
static void tga_film(uint8_t *data)
{
    hdrg_sd_film_grain_t head;
    hdrg_sd_film_grain_av1_t av1;

    memset(&head, 0, sizeof(head));
    memset(&av1, 0, sizeof(av1));
    head.kind = HDRG_FILM_GRAIN_AV1;
    head.bit_depth = 8;
    head.subsampling_x = 1;
    head.subsampling_y = 1;
    av1.num_y_points = 2;
    av1.y_points[0][0] = 16;
    av1.y_points[0][1] = 40;
    av1.y_points[1][0] = 235;
    av1.y_points[1][1] = 20;
    av1.chroma_scaling_from_luma = 1;
    av1.scaling_shift = 8;
    av1.ar_coeff_lag = 1;
    av1.ar_coeffs_y[1] = 40;
    av1.ar_coeffs_y[3] = 40;
    av1.ar_coeff_shift = 7;

    memcpy(data, &head, sizeof(head));
    memcpy(data + sizeof(head), &av1, sizeof(av1));
}

/*
 * a codec's film grain (AV1's parameters, its luma and Cb by the luma's
 * function, correlated) the decoder left out: the same grain whatever the
 * conversion (Y'CbCr too) and on the GPU's frames as on the host's
 */
static void test_film_grain(void)
{
    static const tga_case_t cases[] = {
        {97, 61, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, false,
         false},
        {90, 54, HDRG_PRESET_PHOTO, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
        {720, 405, HDRG_PRESET_GAME, TGA_IN_YUV, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, false,
         false},
    };
    const size_t bytes = tga_out_bytes(&cases[0]) * TGA_FRAMES;
    tga_run_t with, without;
    uint8_t data[HDRG_GRAIN_FILM_BYTES];

    tga_film(data);
    g_film = data;
    tga_cases(cases, (uint32_t)(sizeof(cases) / sizeof(cases[0])));

    /* (the grain there: not the frames without it) */
    with.frames = (uint8_t *)malloc(bytes);
    without.frames = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(with.frames != NULL && without.frames != NULL);
    tga_session(&cases[0], HDRG_GPU_AUTO, &with);
    g_film = NULL;
    tga_session(&cases[0], HDRG_GPU_AUTO, &without);
    TEST_ASSERT_TRUE(memcmp(with.frames, without.frames, bytes) != 0);

    free(with.frames);
    free(without.frames);
}

/* a case's run (gpu, frames on the GPU when it can) as P010: the planar run's codes repacked */
static void tga_packed(const tga_case_t *c, uint8_t gpu)
{
    const size_t frame_bytes = tga_out_bytes(c), pixels = (size_t)c->width * c->height,
                 chroma = (size_t)((c->width + 1u) / 2u) * ((c->height + 1u) / 2u);
    tga_run_t planar, packed;
    uint16_t v, *want;
    uint8_t f;
    size_t i;

    planar.frames = (uint8_t *)malloc(frame_bytes * TGA_FRAMES);
    packed.frames = (uint8_t *)malloc(frame_bytes * TGA_FRAMES);
    want = (uint16_t *)malloc(frame_bytes);
    TEST_ASSERT_TRUE(planar.frames != NULL && packed.frames != NULL && want != NULL);

    g_frames = HDRG_GPU_FRAMES_GPU;
    tga_session(c, gpu, &planar);
    g_packed = true;
    tga_session(c, gpu, &packed);
    g_packed = false;
    g_frames = HDRG_GPU_FRAMES_CHOSEN;
    TEST_ASSERT_EQUAL(planar.resident, packed.resident);
    TEST_ASSERT_EQUAL_MEMORY(planar.nits, packed.nits, sizeof(planar.nits));

    for (f = 0; f < TGA_FRAMES; f++) {
        for (i = 0; i < pixels + 2u * chroma; i++) {
            memcpy(&v, planar.frames + (size_t)f * frame_bytes + 2u * i, sizeof(v));

            if (i < pixels) {
                want[i] = (uint16_t)(v << 6);
            } else if (i < pixels + chroma) {
                want[pixels + 2u * (i - pixels)] = (uint16_t)(v << 6);
            } else {
                want[pixels + 2u * (i - pixels - chroma) + 1u] = (uint16_t)(v << 6);
            }
        }

        TEST_ASSERT_EQUAL_MEMORY(want, packed.frames + (size_t)f * frame_bytes, frame_bytes);
    }

    free(planar.frames);
    free(packed.frames);
    free(want);
}

/*
 * NV12 in and P010 out (Cb and Cr alternating, the samples in the high
 * bits): the planar run's codes repacked, on the CPU and the GPU (the
 * frames on it)
 */
static void test_packed(void)
{
    static const tga_case_t cases[] = {
        {128, 72, HDRG_PRESET_ANIME, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
        {99, 67, HDRG_PRESET_PHOTO, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, true, false},
    };
    uint8_t i;

    for (i = 0; i < 2u; i++) {
        tga_packed(&cases[i], HDRG_GPU_FILTERS);
        tga_packed(&cases[i], HDRG_GPU_AUTO);
    }
}

/*
 * The analysis's sums on the GPU (gpu_analysis FAST): the frames on the GPU
 * (analyzed on a proxy sampled from them, or whole) and in host memory
 * (uploaded), within the GPU's tolerance of the CPU's sums (EXACT), the
 * same twice
 */
static void test_fast_analysis(void)
{
    static const tga_case_t cases[] = {
        {1280, 720, HDRG_PRESET_ANIME, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, true,
         false},
        {99, 67, HDRG_PRESET_PHOTO, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, true, false},
        {160, 96, HDRG_PRESET_PHOTO, TGA_IN_RGB8, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, false,
         false},
    };
    static const uint8_t places[2] = {HDRG_GPU_FRAMES_GPU, HDRG_GPU_FRAMES_HOST};
    tga_run_t exact, fast, again;
    uint32_t a, b, d, worst;
    uint8_t i, k;
    size_t bytes, n, j, off;

    for (i = 0; i < 3u; i++) {
        for (k = 0; k < 2u; k++) {
            bytes = tga_out_bytes(&cases[i]) * TGA_FRAMES;
            exact.frames = (uint8_t *)malloc(bytes);
            fast.frames = (uint8_t *)malloc(bytes);
            again.frames = (uint8_t *)malloc(bytes);
            TEST_ASSERT_TRUE(exact.frames != NULL && fast.frames != NULL && again.frames != NULL);

            g_frames = places[k];
            tga_session(&cases[i], HDRG_GPU_AUTO, &exact);
            g_analysis = HDRG_GPU_ANALYSIS_FAST;
            tga_session(&cases[i], HDRG_GPU_AUTO, &fast);
            tga_session(&cases[i], HDRG_GPU_AUTO, &again);
            g_analysis = HDRG_GPU_ANALYSIS_EXACT;
            g_frames = HDRG_GPU_FRAMES_CHOSEN;

            if (exact.device == 0u) {
                free(exact.frames);
                free(fast.frames);
                free(again.frames);
                TEST_IGNORE_MESSAGE("no GPU for the analysis");
            }

            TEST_ASSERT_EQUAL_MEMORY(fast.frames, again.frames, bytes);

            n = (cases[i].output == TGA_OUT_RGB8) ? bytes : bytes / 2u;
            worst = 0;
            off = 0;

            for (j = 0; j < n; j++) {
                if (cases[i].output == TGA_OUT_RGB8) {
                    a = exact.frames[j];
                    b = fast.frames[j];
                } else {
                    a = (uint32_t)exact.frames[2u * j] | ((uint32_t)exact.frames[2u * j + 1u] << 8);
                    b = (uint32_t)fast.frames[2u * j] | ((uint32_t)fast.frames[2u * j + 1u] << 8);
                }

                d = (a > b) ? a - b : b - a;
                worst = HDRG_TEST_MAX(worst, d);
                off += (d != 0u) ? 1u : 0u;
            }

            TEST_ASSERT_TRUE(worst <= 1u);
            TEST_ASSERT_TRUE(off * 100u <= n);

            free(exact.frames);
            free(fast.frames);
            free(again.frames);
        }
    }
}

/*
 * where a session keeps its frames by itself: on the GPU under WebGPU, and
 * under Metal only with the LOW profile (PLAYBACK reads them in place, its
 * filters' flights overlapping the analysis). OFFLINE's queue (its long
 * steadying window) is deeper than the GPU's frames: on the host (without
 * the temporal filter, as PLAYBACK)
 */
static void test_frames_chosen(void)
{
    static const uint8_t profiles[3] = {HDRG_LATENCY_PLAYBACK, HDRG_LATENCY_LOW,
                                        HDRG_LATENCY_OFFLINE};
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_params_t params;
    hdrg_session_format_t format;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_latency_t report;
    uint8_t i;
    bool expected;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    ctx->gpu_kind = g_kind;

    memset(&format, 0, sizeof(format));
    format.width = 64;
    format.height = 36;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = 64u * 3u;

    for (i = 0; i < 3u; i++) {
        hdrg_session_config_init(&config);
        config.input.layout = HDRG_YUV_420;
        config.input.matrix = HDRG_MATRIX_BT709;
        config.input.bits = 8;
        config.latency = profiles[i];

        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_session_create(ctx, &format, &params, NULL, &config, &session));

        memset(&report, 0, sizeof(report));
        report.struct_size = (uint32_t)sizeof(report);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_latency_report(session, &report));
        hdrg_session_destroy(session);

        if (report.convert_device == 0u) {
            hdrg_context_destroy(ctx);
            TEST_IGNORE_MESSAGE("no GPU");
        }

        expected = profiles[i] == HDRG_LATENCY_LOW ||
                   ((profiles[i] == HDRG_LATENCY_PLAYBACK ||
                     (profiles[i] == HDRG_LATENCY_OFFLINE && !HDRG_BUILTIN_STATS)) &&
                    report.convert_device == HDRG_DEVICE_WEBGPU);
        TEST_ASSERT_EQUAL(expected, report.frames_on_gpu);
    }

    hdrg_context_destroy(ctx);
}

/* two runs' outputs within a code a sample, a percent of the samples off */
static bool tga_near(const tga_case_t *c, const uint8_t *a, const uint8_t *b, size_t bytes)
{
    const bool wide = c->output != TGA_OUT_RGB8;
    uint32_t x, y, d, worst;
    size_t i, n, off;

    n = wide ? bytes / 2u : bytes;
    worst = 0;
    off = 0;

    for (i = 0; i < n; i++) {
        x = wide ? ((uint32_t)a[2u * i] | ((uint32_t)a[2u * i + 1u] << 8)) : a[i];
        y = wide ? ((uint32_t)b[2u * i] | ((uint32_t)b[2u * i + 1u] << 8)) : b[i];
        d = (x > y) ? x - y : y - x;
        worst = HDRG_TEST_MAX(worst, d);
        off += (d != 0u) ? 1u : 0u;
    }

    return worst <= 1u && off * 100u <= n;
}

/*
 * The case's session without a GPU of its own, its state run on a GPU as an
 * app runs it (hdrg_shader's bindings, the core's own conversion and apply programs
 * standing in for the app's pipeline), its outputs packed as tga_session's.
 * false: no GPU
 */
static bool tga_state(const tga_case_t *c, uint8_t *frames)
{
    const uint8_t slots[HDRG_GA_BUFFERS] = {HDRG_GA_SOURCE, HDRG_GA_LUTS,  HDRG_GA_FRAME,
                                            HDRG_GA_DST,    HDRG_GA_LIGHT, HDRG_GA_PLANES},
                  convert_slots[5] = {TGA_GV_YUV, TGA_GV_BASE, TGA_GV_KNOTS, TGA_GV_VALUES,
                                      HDRG_GA_SOURCE};
    const size_t frame_bytes = tga_out_bytes(c), pixels = (size_t)c->width * c->height,
                 chroma = (size_t)((c->width + 1u) / 2u) * ((c->height + 1u) / 2u);
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx, *gpu_ctx;
    hdrg_gpu_core_t *gpu;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_apply_state_t state;
    hdrg_gc_dispatch_t dispatches[3];
    hdrg_status_t status;
    hdrg_yuv_t yuv_in;
    uint32_t sent, received, y, cw, ch;
    uint16_t *converted;
    uint8_t *image, *rgb, *data, *views[HDRG_GA_BUFFERS], *yuv, *dst, i, n;
    size_t row, sizes[HDRG_GA_BUFFERS], convert_sizes[4];

    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &gpu_ctx));

    if (hdrg_gpu_core_create(&gpu_ctx->host, g_kind, &gpu) != HDRG_OK) {
        hdrg_context_destroy(gpu_ctx);

        return false;
    }

    image = hdrg_test_image(c->width, c->height);
    rgb = (uint8_t *)malloc(pixels * 3u);
    data = (uint8_t *)malloc(pixels * 6u * (TGA_FRAMES + 1u));
    converted = (uint16_t *)malloc(pixels * 6u);
    TEST_ASSERT_TRUE(image != NULL && rgb != NULL && data != NULL && converted != NULL);

    desc.gpu = HDRG_GPU_OFF;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));

    memset(&format, 0, sizeof(format));
    format.width = c->width;
    format.height = c->height;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = (c->input == TGA_IN_RGB16) ? 16 : 8;
    format.has_alpha = c->input == TGA_IN_RGBA8;
    format.stride = (size_t)c->width * (format.has_alpha ? 4u : 3u) * (format.bit_depth / 8u);
    row = format.stride;

    hdrg_params_init(&params, c->preset);

    params.mctf_strength = 0.0f; /* the state: nothing changes the frames */
    hdrg_display_caps_init(&caps);
    hdrg_session_config_init(&config);
    config.temporal = c->temporal;
    config.output.bits = (c->output == TGA_OUT_RGB16) ? 16u : 8u;

    if (c->input == TGA_IN_YUV) {
        config.input.layout = HDRG_YUV_420;
        config.input.matrix = HDRG_MATRIX_BT709;
        config.input.bits = 8;
    }

    if (c->output == TGA_OUT_YUV) {
        config.output.layout = HDRG_YUV_420;
        config.output.matrix = HDRG_MATRIX_BT2020;
        config.output.bits = 10;
        caps.gamut = HDRG_GAMUT_BT2020;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, &caps, &config, &session));

    sent = 0;
    received = 0;
    cw = (c->width + 1u) / 2u;
    ch = (c->height + 1u) / 2u;

    for (;;) {
        memset(&out, 0, sizeof(out));
        memset(&state, 0, sizeof(state));
        state.struct_size = (uint32_t)sizeof(state);

        status = hdrg_session_receive_state(session, &out, &state);
        if (status == HDRG_ERR_AGAIN) {
            /* each frame its own data, read until its receive (the plan reads it) */
            if (sent < TGA_FRAMES) {
                tga_rgb(c, image, sent, rgb);
                tga_frame(c, rgb, data + (pixels * 6u * sent), &in);
                in.pts = sent;
                in.film_grain = g_film;
                in.film_grain_size = (g_film != NULL) ? HDRG_GRAIN_FILM_BYTES : 0u;
            }

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TGA_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_EQUAL_UINT32(HDRG_SHADER_LAYOUT, state.layout);
        TEST_ASSERT_TRUE(state.source_pitch >= row);
        TEST_ASSERT_EQUAL((c->input == TGA_IN_YUV), state.convert_params != NULL);

        /* the app: its copy of the frame sent (out.pts), the state's buffers, the shader */
        sizes[HDRG_GA_SOURCE] = state.source_pitch * c->height;
        sizes[HDRG_GA_LUTS] = state.luts_size;
        sizes[HDRG_GA_FRAME] = state.frame_size;
        sizes[HDRG_GA_DST] = state.dst_size;
        sizes[HDRG_GA_LIGHT] = 4u;
        sizes[HDRG_GA_PLANES] = state.planes_size;

        for (i = 0; i < HDRG_GA_BUFFERS; i++) {
            TEST_ASSERT_TRUE(hdrg_gpu_core_buffer(gpu, slots[i], sizes[i], &views[i]));
        }

        tga_rgb(c, image, (uint32_t)out.pts, rgb);
        tga_frame(c, rgb, data + (pixels * 6u * TGA_FRAMES), &in);

        /* Y'CbCr: the planes packed as the state says, converted into the source on the GPU */
        if (state.convert_params != NULL) {
            convert_sizes[0] = state.yuv_size;
            convert_sizes[1] = state.base_size;
            convert_sizes[2] = state.knots_size;
            convert_sizes[3] = state.values_size;

            for (i = 0; i < 4u; i++) {
                TEST_ASSERT_TRUE(hdrg_gpu_core_buffer(gpu, convert_slots[i], convert_sizes[i],
                                                      (i == 0u) ? &yuv : NULL));
            }

            for (y = 0; y < c->height; y++) {
                memcpy(yuv + (size_t)y * state.yuv_y_pitch,
                       in.planes[0] + (size_t)y * in.strides[0], c->width);
            }

            for (y = 0; y < ch; y++) {
                memcpy(yuv + state.yuv_cb_at + (size_t)y * state.yuv_c_pitch,
                       in.planes[1] + (size_t)y * in.strides[1], cw);
                memcpy(yuv + state.yuv_cr_at + (size_t)y * state.yuv_c_pitch,
                       in.planes[2] + (size_t)y * in.strides[2], cw);
            }

            n = 0;

            for (i = 0; i < 3u; i++) {
                if (state.convert_groups[i][0] != 0u) {
                    dispatches[n].entry = i;
                    dispatches[n].groups_x = state.convert_groups[i][0];
                    dispatches[n].groups_y = state.convert_groups[i][1];
                    n++;
                }
            }

            TEST_ASSERT_TRUE(hdrg_gpu_core_run(gpu, HDRG_GC_CONVERT, state.convert_params,
                                               convert_slots, dispatches, n, 1u << TGA_GV_YUV,
                                               1u << HDRG_GA_SOURCE));

            /* the library's own conversion of the frame on the GPU, bit for bit */
            memset(&yuv_in, 0, sizeof(yuv_in));
            yuv_in.width = c->width;
            yuv_in.height = c->height;
            yuv_in.layout = HDRG_YUV_420;
            yuv_in.matrix = HDRG_MATRIX_BT709;
            yuv_in.bits = 8;
            yuv_in.fit = params.chroma_fit;

            for (i = 0; i < 3u; i++) {
                yuv_in.planes[i] = in.planes[i];
                yuv_in.strides[i] = in.strides[i];
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_gpu_convert_frame(gpu, NULL, &yuv_in, converted,
                                                                  (size_t)c->width * 6u,
                                                                  HDRG_GC_SLOTS, NULL));

            for (y = 0; y < c->height; y++) {
                TEST_ASSERT_EQUAL_MEMORY(converted + (size_t)y * c->width * 3u,
                                         views[HDRG_GA_SOURCE] + (size_t)y * state.source_pitch,
                                         (size_t)c->width * 6u);
            }
        } else {
            for (y = 0; y < c->height; y++) {
                memcpy(views[HDRG_GA_SOURCE] + (size_t)y * state.source_pitch,
                       data + (pixels * 6u * TGA_FRAMES) + ((size_t)y * row), row);
            }
        }

        memcpy(views[HDRG_GA_LUTS], state.luts, state.luts_size);
        memcpy(views[HDRG_GA_FRAME], state.frame, state.frame_size);
        memset(views[HDRG_GA_LIGHT], 0, 4u);
        dispatches[0].entry = 0;
        dispatches[0].groups_x = state.groups[0];
        dispatches[0].groups_y = state.groups[1];
        dispatches[1].entry = 1;
        dispatches[1].groups_x = state.yuv_groups[0];
        dispatches[1].groups_y = state.yuv_groups[1];

        TEST_ASSERT_TRUE(hdrg_gpu_core_run(
            gpu, HDRG_GC_APPLY, state.params, slots, dispatches,
            (state.yuv_groups[0] != 0u) ? 2u : 1u,
            ((state.convert_params == NULL) ? (1u << HDRG_GA_SOURCE) : 0u) | (1u << HDRG_GA_LUTS) |
                (1u << HDRG_GA_FRAME) | (1u << HDRG_GA_LIGHT),
            (1u << HDRG_GA_DST) | (1u << HDRG_GA_PLANES)));

        /* packed as tga_session's */
        dst = frames + (size_t)received * frame_bytes;

        if (c->output == TGA_OUT_YUV) {
            for (y = 0; y < c->height; y++) {
                memcpy(dst + (size_t)y * c->width * 2u,
                       views[HDRG_GA_PLANES] + (size_t)y * state.y_pitch, (size_t)c->width * 2u);
            }

            for (y = 0; y < ch; y++) {
                memcpy(dst + pixels * 2u + (size_t)y * cw * 2u,
                       views[HDRG_GA_PLANES] + state.cb_at + (size_t)y * state.c_pitch,
                       (size_t)cw * 2u);
                memcpy(dst + (pixels + chroma) * 2u + (size_t)y * cw * 2u,
                       views[HDRG_GA_PLANES] + state.cr_at + (size_t)y * state.c_pitch,
                       (size_t)cw * 2u);
            }
        } else {
            for (y = 0; y < c->height; y++) {
                memcpy(dst + (y * (frame_bytes / c->height)),
                       views[HDRG_GA_DST] + (size_t)y * state.dst_pitch, frame_bytes / c->height);
            }
        }

        received++;
    }

    TEST_ASSERT_EQUAL_UINT32(TGA_FRAMES, received);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    hdrg_gpu_core_destroy(gpu);
    hdrg_context_destroy(gpu_ctx);
    free(image);
    free(rgb);
    free(data);
    free(converted);

    return true;
}

/*
 * the state of hdrg_session_receive_state run with the shader as an app
 * would gives the GPU apply's output; sessions whose frames something
 * changes give none
 */
static void test_state(void)
{
    static const tga_case_t cases[] = {
        {97, 61, HDRG_PRESET_GAME, TGA_IN_RGB8, TGA_OUT_RGB16, HDRG_TEMPORAL_WINDOWED, false,
         false},
        {80, 48, HDRG_PRESET_GAME, TGA_IN_RGBA8, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
        {64, 40, HDRG_PRESET_GAME, TGA_IN_RGB16, TGA_OUT_RGB8, HDRG_TEMPORAL_STATIC, false, false},
        {96, 54, HDRG_PRESET_GAME, TGA_IN_YUV, TGA_OUT_YUV, HDRG_TEMPORAL_WINDOWED, false, false},
        {65, 37, HDRG_PRESET_GAME, TGA_IN_YUV, TGA_OUT_RGB16, HDRG_TEMPORAL_STATIC, false, false},
    };
    const uint32_t *words;
    static const char *const entries[2][3] = {{"apply", "yuv", NULL},
                                              {"sites", "slopes", "pixels"}};
    const char *text;
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_output_t out;
    hdrg_apply_state_t state;
    hdrg_status_t changed;
    tga_run_t gpu;
    uint8_t *frames, i, m, film[HDRG_GRAIN_FILM_BYTES];
    size_t bytes, size;

    /* the shaders: each module's WGSL, each entry point's SPIR-V and HLSL */
    for (m = 0; m < 2u; m++) {
        text = (const char *)hdrg_shader(HDRG_SHADER_WGSL, m + 1u, NULL, &size);
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_EQUAL_size_t(strlen(text), size);
        TEST_ASSERT_NOT_NULL(strstr(text, (m == 0u) ? "fn apply(" : "fn pixels("));

        for (i = 0; i < 3u && entries[m][i] != NULL; i++) {
            words = (const uint32_t *)hdrg_shader(HDRG_SHADER_SPIRV, m + 1u, entries[m][i], &size);
            TEST_ASSERT_NOT_NULL(words);
            TEST_ASSERT_TRUE(size > 20u && size % 4u == 0u);
            TEST_ASSERT_EQUAL_HEX32(0x07230203u, words[0]);
            text = (const char *)hdrg_shader(HDRG_SHADER_HLSL, m + 1u, entries[m][i], &size);
            TEST_ASSERT_NOT_NULL(text);
            TEST_ASSERT_EQUAL_size_t(strlen(text), size);
            TEST_ASSERT_NOT_NULL(strstr(text, "[numthreads(64, 1, 1)]"));
        }
    }

    TEST_ASSERT_NULL(hdrg_shader(HDRG_SHADER_SPIRV, HDRG_SHADER_APPLY, "pixels", &size));
    TEST_ASSERT_NULL(hdrg_shader(HDRG_SHADER_HLSL, HDRG_SHADER_CONVERT, NULL, &size));
    TEST_ASSERT_NULL(hdrg_shader(9u, HDRG_SHADER_APPLY, "apply", &size));
    TEST_ASSERT_NULL(hdrg_shader(HDRG_SHADER_WGSL, 3u, NULL, &size));

    for (i = 0; i < 5u; i++) {
        bytes = tga_out_bytes(&cases[i]) * TGA_FRAMES;
        frames = (uint8_t *)calloc(1, bytes);
        gpu.frames = (uint8_t *)malloc(bytes);
        TEST_ASSERT_TRUE(frames != NULL && gpu.frames != NULL);

        if (!tga_state(&cases[i], frames)) {
            free(frames);
            free(gpu.frames);
            TEST_IGNORE_MESSAGE("no GPU");
        }

        tga_session(&cases[i], HDRG_GPU_AUTO, &gpu);

        /* Y'CbCr: the library's analysis reads its GPU's conversion, the app's session its CPU's */
        if (cases[i].input == TGA_IN_YUV) {
            TEST_ASSERT_TRUE(tga_near(&cases[i], gpu.frames, frames, bytes));
        } else {
            TEST_ASSERT_EQUAL_MEMORY(gpu.frames, frames, bytes);
        }

        free(frames);
        free(gpu.frames);
    }

    /* a codec's film grain in the frames' words: the app's apply adds it as the library's */
    tga_film(film);
    g_film = film;
    bytes = tga_out_bytes(&cases[0]) * TGA_FRAMES;
    frames = (uint8_t *)calloc(1, bytes);
    gpu.frames = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && gpu.frames != NULL);
    TEST_ASSERT_TRUE(tga_state(&cases[0], frames));
    tga_session(&cases[0], HDRG_GPU_AUTO, &gpu);
    g_film = NULL;
    TEST_ASSERT_EQUAL_MEMORY(gpu.frames, frames, bytes);
    free(frames);
    free(gpu.frames);

    /* PREFILTER filters change the frame (PHOTO's, where the build has them), R'G'B' and Y'CbCr
       alike */
    changed = (HDRG_BUILTIN_DEBLOCK != 0 || HDRG_BUILTIN_DEBAND != 0) ? HDRG_ERR_UNSUPPORTED
                                                                      : HDRG_ERR_AGAIN;
    hdrg_context_desc_init(&desc);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    memset(&format, 0, sizeof(format));
    format.width = 64;
    format.height = 32;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = 64u * 3u;
    hdrg_session_config_init(&config);
    memset(&state, 0, sizeof(state));
    state.struct_size = (uint32_t)sizeof(state);

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));
    TEST_ASSERT_EQUAL_INT(changed, hdrg_session_receive_state(session, &out, &state));
    hdrg_session_destroy(session);

    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 8;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));
    TEST_ASSERT_EQUAL_INT(changed, hdrg_session_receive_state(session, &out, &state));
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* whether the build and the system have a GPU of the kind for the core */
static bool tga_available(uint32_t kind)
{
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_gpu_core_t *gpu;
    bool available;

    hdrg_context_desc_init(&desc);

    if (hdrg_context_create(&desc, &ctx) != HDRG_OK) {
        return false;
    }

    available = hdrg_gpu_core_create(&ctx->host, kind, &gpu) == HDRG_OK;
    hdrg_gpu_core_destroy(available ? gpu : NULL);
    hdrg_context_destroy(ctx);

    return available;
}

void setUp(void)
{
}

void tearDown(void)
{
    /* (a test ignored or failed on the way leaves none of its own) */
    g_film = NULL;
    g_preserve = false;
    g_packed = false;
    g_yuv_bits = 10;
}

int main(void)
{
    static const uint32_t kinds[3] = {HDRG_DEVICE_METAL, HDRG_DEVICE_VULKAN, HDRG_DEVICE_WEBGPU};
    uint8_t k, ran;

    UNITY_BEGIN();
    ran = 0;

    /* each GPU of the build's and the system's in turn (none: the tests ignored) */
    for (k = 0; k < 4u; k++) {
        g_kind = (k < 3u) ? kinds[k] : 0u;

        if ((k < 3u && !tga_available(g_kind)) || (k == 3u && ran > 0)) {
            continue;
        }

        ran++;
        RUN_TEST(test_rgb);
        RUN_TEST(test_yuv);
        RUN_TEST(test_yuv8);
        RUN_TEST(test_cored_and_fixed);
        RUN_TEST(test_film_grain);
        RUN_TEST(test_packed);
        RUN_TEST(test_fast_analysis);
        RUN_TEST(test_frames_chosen);
        RUN_TEST(test_state);
    }

    return UNITY_END();
}
