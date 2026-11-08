/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "core/motion.h"
#include "hdrgrading/plugin.h"
#include "hdrgrading/video.h"

/*
 * The temporal noise reduction hdrg.mctf in a video
 * session: a panning texture with noise comes nearer to the clean one's
 * output than without it; a sharp moving bar too (no ghost); a cut is not
 * blended across; the same bytes whatever its threads and as the built-in
 * chain's; a seek with the preroll the same bytes; its delay (none under
 * LOW); a still converts as without it.
 */

#define TM_W      192u
#define TM_H      128u
#define TM_FRAMES 12u
#define TM_VX     3.0 /* the texture's motion, pixels a frame */
#define TM_VY     1.0
#define TM_SIGMA  4.0   /* the noise, 8-bit codes */
#define TM_MARGIN 12u   /* the pixels from the edges the measures skip */
#define TM_MOTION 4096u /* an output's HDRG_SD_MOTION at most (4 fields of 12 x 8 blocks) */

static const char *g_gpu;                /* tm_run: a GPU version added instead (NULL: none) */
static int g_future = -1;                /* tm_run: the added filter's future (< 0: its default) */
static uint8_t *g_motions;               /* tm_run: the outputs' motion (TM_MOTION each; NULL) */
static size_t g_motion_sizes[TM_FRAMES]; /* ... their bytes */

/* the session's frame k: the texture panned, a bright bar moving 5 pixels a frame, a cut at cut */
static void tm_frame(uint8_t *rgb, uint32_t k, double sigma, uint32_t cut, bool bar)
{
    uint32_t x, y, h;
    uint8_t c;
    double u, v, t, n;

    for (y = 0; y < TM_H; y++) {
        for (x = 0; x < TM_W; x++) {
            u = (double)x - TM_VX * (double)k;
            v = (double)y - TM_VY * (double)k;
            t = 0.45 + 0.15 * sin(u * 0.21) * cos(v * 0.17) + 0.08 * sin((u + v) * 0.09);

            if (k >= cut) {
                t = 0.75 + 0.1 * cos(u * 0.13 - v * 0.31);
            }

            if (bar && x >= 20u + 5u * k && x < 32u + 5u * k && y >= 40u && y < 90u) {
                t = 0.9;
            }

            for (c = 0; c < 3u; c++) {
                h = hdrg_hash_pixel(hdrg_hash_row(UINT64_C(0x6d637466) + k * 3u + c, y), x);
                n = ((double)(h & 0xffu) + (double)((h >> 8) & 0xffu) +
                     (double)((h >> 16) & 0xffu) + (double)(h >> 24) - 510.0) /
                    (255.0 * 0.57735);
                rgb[((size_t)y * TM_W + x) * 3u + c] =
                    (uint8_t)fmin(fmax(t * (0.8 + 0.1 * c) * 255.0 + sigma * n + 0.5, 0.0), 255.0);
            }
        }
    }
}

/*
 * the frames [start, TM_FRAMES) through a session (16-bit PQ out) of params
 * at mctf (the PREFILTER chain hdrg.mctf alone; filter: added instead, on a
 * context of filter threads; g_gpu: that class added instead), preroll from
 * start: the outputs into out (*count UINT32_MAX: no such class or device)
 */
static void tm_run(double sigma, uint32_t cut, bool bar, float mctf, int filter, uint8_t latency,
                   uint32_t start, uint32_t preroll, uint16_t *out, uint32_t *count,
                   uint32_t *latency_frames)
{
    const size_t frame_samples = (size_t)TM_W * TM_H * 3u;
    hdrg_context_t *ctx;
    hdrg_options_t *options;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t frame;
    hdrg_session_output_t output;
    hdrg_status_t status;
    uint32_t sent;
    uint8_t *pixels;
    char value[16];

    pixels = (uint8_t *)malloc(frame_samples * TM_FRAMES);
    TEST_ASSERT_NOT_NULL(pixels);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK,
        hdrg_test_context(&ctx, NULL, (filter >= 0 && g_gpu == NULL) ? (uint32_t)filter : 0u));

    options = NULL;
    snprintf(value, sizeof(value), "%d", g_future);

    if (g_gpu != NULL && g_future >= 0) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "future", value));
    }

    status = (g_gpu != NULL) ? hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, g_gpu, options)
                             : HDRG_OK;
    hdrg_options_destroy(options);

    if (status != HDRG_OK) {
        hdrg_context_destroy(ctx);
        free(pixels);
        *count = UINT32_MAX;

        return;
    }

    if (filter >= 0 && g_gpu == NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
        snprintf(value, sizeof(value), "%d", g_future);

        if (g_future >= 0) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "future", value));
        }

        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, "hdrg.mctf", options));
        hdrg_options_destroy(options);
    }

    memset(&format, 0, sizeof(format));
    format.width = TM_W;
    format.height = TM_H;
    format.bit_depth = 8;
    format.stride = (size_t)TM_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.grain_policy = HDRG_GRAIN_PRESERVE; /* MCTF's own (test_grain: RESYNTH) */
    params.deblock_strength = 0.0f;
    params.deband_strength = 0.0f;
    params.mctf_strength = mctf;
    hdrg_session_config_init(&config);
    config.output.bits = 16;
    config.dither_off = true;
    config.latency = latency;
    config.rate_num = 30;
    config.rate_den = 1;
    config.start = start;
    config.preroll = preroll;

    status = hdrg_session_create(ctx, &format, &params, NULL, &config, &session);
    if (status != HDRG_OK && g_gpu != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, status);
        hdrg_context_destroy(ctx);
        free(pixels);
        *count = UINT32_MAX;

        return;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, status);

    if (latency_frames != NULL) {
        *latency_frames = hdrg_session_latency(session);
    }

    sent = start;
    *count = 0;

    for (;;) {
        memset(&output, 0, sizeof(output));
        output.planes[0] = (uint8_t *)(out + (size_t)*count * frame_samples);
        output.strides[0] = (size_t)TM_W * 6u;

        status = hdrg_session_receive_frame(session, &output);
        if (status == HDRG_ERR_AGAIN) {
            /* a frame of its own: a session without a PREFILTER chain reads it until released */
            if (sent < TM_FRAMES) {
                tm_frame(pixels + (size_t)sent * frame_samples, sent, sigma, cut, bar);
            }

            memset(&frame, 0, sizeof(frame));
            frame.planes[0] = pixels + (size_t)((sent < TM_FRAMES) ? sent : 0u) * frame_samples;
            frame.pts = (int64_t)sent;

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TM_FRAMES) ? &frame : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(*count < TM_FRAMES);

        /* the output's motion (hdrg.mctf's fields) */
        if (g_motions != NULL) {
            TEST_ASSERT_TRUE(output.motion_size <= TM_MOTION);
            TEST_ASSERT_TRUE(output.motion == NULL ||
                             hdrg_motion_check(output.motion, output.motion_size));
            g_motion_sizes[*count] = output.motion_size;

            if (output.motion != NULL) {
                memcpy(g_motions + (size_t)*count * TM_MOTION, output.motion, output.motion_size);
            }
        }

        (*count)++;
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(pixels);
}

/* PSNR (16-bit) of frame k of got against want over the box [x0, x1) x [y0, y1) */
static double tm_psnr(const uint16_t *got, const uint16_t *want, uint32_t k, uint32_t x0,
                      uint32_t x1, uint32_t y0, uint32_t y1)
{
    const size_t base = (size_t)k * TM_W * TM_H * 3u;
    uint32_t x, y;
    uint8_t c;
    size_t i, n;
    double sum, d;

    sum = 0.0;
    n = 0;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            for (c = 0; c < 3u; c++) {
                i = base + ((size_t)y * TM_W + x) * 3u + c;
                d = (double)got[i] - (double)want[i];
                sum += d * d;
                n++;
            }
        }
    }

    return 10.0 * log10(65535.0 * 65535.0 / HDRG_TEST_MAX(sum / (double)n, 1e-9));
}

static uint16_t *tm_frames(void)
{
    uint16_t *frames = (uint16_t *)malloc((size_t)TM_W * TM_H * 3u * TM_FRAMES * sizeof(uint16_t));

    TEST_ASSERT_NOT_NULL(frames);

    return frames;
}

/*
 * the noisy texture's frames with a full window, and the moving bar's
 * neighborhood, nearer to the clean frames' output than without hdrg.mctf
 */
static void test_reduces_noise(void)
{
    uint32_t k, count;
    uint16_t *clean, *noisy, *filtered;
    char line[160];
    double before, after, bar_before, bar_after, worst;

    clean = tm_frames();
    noisy = tm_frames();
    filtered = tm_frames();
    tm_run(0.0, UINT32_MAX, true, 0.0f, -1, HDRG_LATENCY_PLAYBACK, 0, 0, clean, &count, NULL);
    TEST_ASSERT_EQUAL_UINT32(TM_FRAMES, count);
    tm_run(TM_SIGMA, UINT32_MAX, true, 0.0f, -1, HDRG_LATENCY_PLAYBACK, 0, 0, noisy, &count, NULL);
    tm_run(TM_SIGMA, UINT32_MAX, true, 1.0f, -1, HDRG_LATENCY_PLAYBACK, 0, 0, filtered, &count,
           NULL);
    TEST_ASSERT_EQUAL_UINT32(TM_FRAMES, count);
    worst = 100.0;

    for (k = 2; k + 2u < TM_FRAMES; k++) {
        before = tm_psnr(noisy, clean, k, TM_MARGIN, TM_W - TM_MARGIN, TM_MARGIN, TM_H - TM_MARGIN);
        after =
            tm_psnr(filtered, clean, k, TM_MARGIN, TM_W - TM_MARGIN, TM_MARGIN, TM_H - TM_MARGIN);
        bar_before = tm_psnr(noisy, clean, k, 14u + 5u * k, 38u + 5u * k, 34u, 96u);
        bar_after = tm_psnr(filtered, clean, k, 14u + 5u * k, 38u + 5u * k, 34u, 96u);
        snprintf(line, sizeof(line), "frame %u: %.1f -> %.1f dB, around the bar %.1f -> %.1f dB", k,
                 before, after, bar_before, bar_after);
        TEST_MESSAGE(line);

        TEST_ASSERT_TRUE(after > before + 3.0);
        TEST_ASSERT_TRUE(bar_after > bar_before);
        worst = fmin(worst, after - before);
    }

    snprintf(line, sizeof(line), "the least gain %.1f dB", worst);
    TEST_MESSAGE(line);

    free(clean);
    free(noisy);
    free(filtered);
}

/* the first frame after a cut keeps its own content: no ghost of the frames before */
static void test_cut(void)
{
    const uint32_t cut = 6;
    uint32_t count;
    uint16_t *noisy, *filtered;
    char line[96];
    double before, after;

    noisy = tm_frames();
    filtered = tm_frames();
    tm_run(TM_SIGMA, cut, false, 0.0f, -1, HDRG_LATENCY_PLAYBACK, 0, 0, noisy, &count, NULL);
    tm_run(TM_SIGMA, cut, false, 1.0f, -1, HDRG_LATENCY_PLAYBACK, 0, 0, filtered, &count, NULL);
    before = tm_psnr(filtered, noisy, cut - 1u, 0, TM_W, 0, TM_H);
    after = tm_psnr(filtered, noisy, cut, 0, TM_W, 0, TM_H);
    snprintf(line, sizeof(line), "around the cut: against the noisy frames %.1f and %.1f dB",
             before, after);
    TEST_MESSAGE(line);

    /* within the noise of the frame itself: well above a mix of the other scene (~ 15 dB) */
    TEST_ASSERT_TRUE(before > 30.0 && after > 30.0);

    free(noisy);
    free(filtered);
}

/* the same bytes whatever its threads, and as the params' built-in chain */
static void test_threads(void)
{
    const size_t bytes = (size_t)TM_W * TM_H * 3u * TM_FRAMES * sizeof(uint16_t);
    uint32_t count;
    uint16_t *one, *eight, *builtin;

    HDRG_TEST_REQUIRE_THREADS();

    one = tm_frames();
    eight = tm_frames();
    builtin = tm_frames();
    tm_run(TM_SIGMA, UINT32_MAX, true, 0.0f, 1, HDRG_LATENCY_PLAYBACK, 0, 0, one, &count, NULL);
    tm_run(TM_SIGMA, UINT32_MAX, true, 0.0f, 8, HDRG_LATENCY_PLAYBACK, 0, 0, eight, &count, NULL);
    tm_run(TM_SIGMA, UINT32_MAX, true, 1.0f, -1, HDRG_LATENCY_PLAYBACK, 0, 0, builtin, &count,
           NULL);
    TEST_ASSERT_EQUAL_MEMORY(one, eight, bytes);
    TEST_ASSERT_EQUAL_MEMORY(one, builtin, bytes);

    free(one);
    free(eight);
    free(builtin);
}

/*
 * opened again at frame n - preroll its outputs from n the same bytes;
 * 2 frames more of delay, none under LOW
 */
static void test_seek_and_delay(void)
{
    const size_t frame_samples = (size_t)TM_W * TM_H * 3u;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    uint32_t count, preroll, n, on, off, low_on, low_off;
    uint16_t *whole, *part;

    whole = tm_frames();
    part = tm_frames();
    tm_run(TM_SIGMA, UINT32_MAX, true, 1.0f, -1, HDRG_LATENCY_OFFLINE, 0, 0, whole, &count, &on);
    TEST_ASSERT_EQUAL_UINT32(TM_FRAMES, count);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    memset(&format, 0, sizeof(format));
    format.width = TM_W;
    format.height = TM_H;
    format.bit_depth = 8;
    format.stride = (size_t)TM_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.grain_policy = HDRG_GRAIN_PRESERVE; /* MCTF's own (test_grain: RESYNTH) */
    params.deblock_strength = 0.0f;
    params.deband_strength = 0.0f;
    hdrg_session_config_init(&config);
    config.latency = HDRG_LATENCY_OFFLINE;
    config.rate_num = 30;
    config.rate_den = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &config, &session));
    preroll = hdrg_session_preroll(session);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    /* the frames after 2 at most: the preroll reaches back to the stream's start */
    n = 8;
    preroll = (preroll > n) ? n : preroll;
    tm_run(TM_SIGMA, UINT32_MAX, true, 1.0f, -1, HDRG_LATENCY_OFFLINE, n - preroll, preroll, part,
           &count, NULL);
    TEST_ASSERT_EQUAL_UINT32(TM_FRAMES - n, count);
    TEST_ASSERT_EQUAL_MEMORY(whole + (size_t)n * frame_samples, part,
                             (size_t)(TM_FRAMES - n) * frame_samples * sizeof(uint16_t));

    tm_run(TM_SIGMA, UINT32_MAX, true, 0.0f, -1, HDRG_LATENCY_OFFLINE, 0, 0, part, &count, &off);
    tm_run(TM_SIGMA, UINT32_MAX, true, 1.0f, -1, HDRG_LATENCY_LOW, 0, 0, part, &count, &low_on);
    tm_run(TM_SIGMA, UINT32_MAX, true, 0.0f, -1, HDRG_LATENCY_LOW, 0, 0, part, &count, &low_off);
    TEST_ASSERT_EQUAL_UINT32(off + 2u, on);
    TEST_ASSERT_EQUAL_UINT32(low_off, low_on);

    free(whole);
    free(part);
}

/*
 * the output's motion (HDRG_SD_MOTION): hdrg.mctf's fields
 * to the frames of its window in its order (-1, +1, -2, +2; the ends with
 * fewer), the panning texture's (3, 1) pixels a frame on most blocks
 */
static void test_motion(void)
{
    const uint8_t *side, *at;
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field;
    hdrg_sd_motion_block_t block;
    uint32_t count, f, k, i, exact, sure, n;
    uint16_t *out;
    uint8_t *motions;
    int32_t want[HDRG_SD_MOTION_FIELDS];
    char line[128];

    out = tm_frames();
    motions = (uint8_t *)malloc((size_t)TM_MOTION * TM_FRAMES);
    TEST_ASSERT_NOT_NULL(motions);
    g_motions = motions;
    tm_run(TM_SIGMA, UINT32_MAX, false, 1.0f, -1, HDRG_LATENCY_PLAYBACK, 0, 0, out, &count, NULL);
    g_motions = NULL;
    TEST_ASSERT_EQUAL_UINT32(TM_FRAMES, count);

    for (f = 0; f < TM_FRAMES; f++) {
        side = motions + (size_t)f * TM_MOTION;
        TEST_ASSERT_TRUE(g_motion_sizes[f] > 0u);
        memcpy(&head, side, sizeof(head));
        n = 0;

        /* the window's frames there are, nearer first, before then after */
        for (k = 1; k <= 2u; k++) {
            if (f >= k) {
                want[n++] = -(int32_t)k;
            }

            if (f + k < TM_FRAMES) {
                want[n++] = (int32_t)k;
            }
        }

        TEST_ASSERT_EQUAL_UINT32(n, head.fields);
        at = side + sizeof(head);

        for (k = 0; k < head.fields; k++) {
            memcpy(&field, at, sizeof(field));
            TEST_ASSERT_EQUAL_UINT32(HDRG_MOTION_INTERNAL, field.source);
            TEST_ASSERT_EQUAL_INT32(want[k], field.ref_offset);
            TEST_ASSERT_EQUAL_UINT32(16, field.block_w);
            TEST_ASSERT_EQUAL_UINT32(0, field.phase);
            exact = 0;
            sure = 0;

            for (i = 0; i < field.grid_w * field.grid_h; i++) {
                memcpy(&block, at + sizeof(field) + i * sizeof(block), sizeof(block));
                exact += (block.mv_x == 4 * 3 * field.ref_offset &&
                          block.mv_y == 4 * 1 * field.ref_offset)
                             ? 1u
                             : 0u;
                sure += (block.confidence >= 128u) ? 1u : 0u;
            }

            if (exact * 10u < field.grid_w * field.grid_h * 8u ||
                sure * 10u < field.grid_w * field.grid_h * 8u) {
                snprintf(line, sizeof(line), "frame %u, %+d: %u exact, %u sure", f, want[k], exact,
                         sure);
                TEST_FAIL_MESSAGE(line);
            }

            if (f == TM_FRAMES / 2u) {
                snprintf(line, sizeof(line), "frame %u, %+d: %u of %u blocks exact, %u sure", f,
                         want[k], exact, field.grid_w * field.grid_h, sure);
                TEST_MESSAGE(line);
            }

            at += sizeof(field) + (size_t)field.grid_w * field.grid_h * sizeof(block);
        }
    }

    free(out);
    free(motions);
}

/*
 * the GPU versions (Metal, Vulkan, WebGPU: those of the build with a device)
 * give the CPU's bytes: the panning texture and its bar, the cut, and the
 * output's motion
 */
static void test_gpu(void)
{
    const size_t bytes = (size_t)TM_W * TM_H * 3u * TM_FRAMES * sizeof(uint16_t);
    static const char *const ids[3] = {"hdrg.metal.mctf", "hdrg.vulkan.mctf", "hdrg.webgpu.mctf"};
    uint32_t count, cut, f;
    uint16_t *cpu, *gpu;
    uint8_t i, ran, *motions;
    size_t sizes[TM_FRAMES];
    char line[96];

    cpu = tm_frames();
    gpu = tm_frames();
    motions = (uint8_t *)malloc((size_t)TM_MOTION * TM_FRAMES * 2u);
    TEST_ASSERT_NOT_NULL(motions);
    ran = 0;

    /* the pattern, the cut, and LOW without the future (the first frame's window empty) */
    for (i = 0; i < 3u; i++) {
        for (cut = 0; cut < 3u; cut++) {
            g_gpu = NULL;
            g_motions = motions;
            g_future = (cut == 2u) ? 0 : -1;
            tm_run(TM_SIGMA, (cut == 1u) ? 6u : UINT32_MAX, cut != 1u, 0.0f, 4,
                   (cut == 2u) ? HDRG_LATENCY_LOW : HDRG_LATENCY_PLAYBACK, 0, 0, cpu, &count, NULL);
            memcpy(sizes, g_motion_sizes, sizeof(sizes));
            g_gpu = ids[i];
            g_motions = motions + (size_t)TM_MOTION * TM_FRAMES;
            tm_run(TM_SIGMA, (cut == 1u) ? 6u : UINT32_MAX, cut != 1u, 0.0f, 0,
                   (cut == 2u) ? HDRG_LATENCY_LOW : HDRG_LATENCY_PLAYBACK, 0, 0, gpu, &count, NULL);
            g_gpu = NULL;
            g_motions = NULL;
            g_future = -1;

            if (count == UINT32_MAX) {
                break;
            }

            /* the frames and their motion (the side data) */
            TEST_ASSERT_EQUAL_UINT32(TM_FRAMES, count);
            TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, bytes);
            TEST_ASSERT_EQUAL_MEMORY(sizes, g_motion_sizes, sizeof(sizes));

            /* LOW's first frame: an empty window, no motion */
            for (f = (cut == 2u) ? 1u : 0u; f < TM_FRAMES; f++) {
                TEST_ASSERT_TRUE(sizes[f] > 0u);
                TEST_ASSERT_EQUAL_MEMORY(motions + (size_t)f * TM_MOTION,
                                         motions + (size_t)(TM_FRAMES + f) * TM_MOTION, sizes[f]);
            }
        }

        if (count != UINT32_MAX) {
            snprintf(line, sizeof(line), "%s: the CPU's bytes", ids[i]);
            TEST_MESSAGE(line);
            ran++;
        }
    }

    free(cpu);
    free(gpu);
    free(motions);

    if (ran == 0) {
        TEST_IGNORE_MESSAGE("no GPU version of hdrg.mctf with a device");
    }
}

/* a still has no frames around it: converted as without hdrg.mctf */
static void test_still(void)
{
    hdrg_context_t *ctx;
    hdrg_raw_image_t raw;
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    uint8_t *pixels, *with, *without;
    size_t with_size, without_size;

    pixels = (uint8_t *)malloc((size_t)TM_W * TM_H * 3u);
    TEST_ASSERT_NOT_NULL(pixels);
    tm_frame(pixels, 3, TM_SIGMA, UINT32_MAX, true);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    memset(&raw, 0, sizeof(raw));
    raw.width = TM_W;
    raw.height = TM_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TM_W * 3u;
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    hdrg_encode_params_init(&encode);
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.grain_policy = HDRG_GRAIN_PRESERVE; /* MCTF's own (test_grain: RESYNTH) */
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, &with, &with_size));
    params.mctf_strength = 0.0f;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, &without, &without_size));
    TEST_ASSERT_EQUAL_size_t(without_size, with_size);
    TEST_ASSERT_EQUAL_MEMORY(without, with, with_size);

    hdrg_free(with);
    hdrg_free(without);
    hdrg_context_destroy(ctx);
    free(pixels);
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
    RUN_TEST(test_reduces_noise);
    RUN_TEST(test_cut);
    RUN_TEST(test_threads);
    RUN_TEST(test_seek_and_delay);
    RUN_TEST(test_motion);
    RUN_TEST(test_gpu);
    RUN_TEST(test_still);

    return UNITY_END();
}
