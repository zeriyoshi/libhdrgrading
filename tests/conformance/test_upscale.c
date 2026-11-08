/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../test_stage.h"
#include "../test_util.h"
#include "hdrgrading/filter.h"
#include "hdrgrading/video.h"
#include "io/io.h"
#include "host/host.h"
#include "host/stage.h"
#include "priv.h"

/*
 * hdrg.scale.fsr1 (AMD FidelityFX Super Resolution 1, src/plugins/fsr/fsr.h):
 * any band split and thread count give the same output (EASU and RCAS);
 * a flat picture stays flat (EASU exactly, RCAS within a code); EASU stays
 * within the source's range; a smaller output is refused and the same size
 * is the source; a disc's anti-aliased edge
 * upscaled twice is closer to the disc drawn at that size than bilinear's;
 * RCAS sharpens it. The GPU versions (Metal, Vulkan, WebGPU: those with a
 * device) are within 2 codes of the CPU's.
 */
#define TF_W     96
#define TF_H     64
#define TF_SUPER 8 /* the disc drawn at 8 x 8 samples a pixel of the largest size */

static hdrg_allocator_t g_alloc;
static uint16_t *g_pixels;                      /* the source, RGB16 ... */
static uint32_t g_src_w = TF_W, g_src_h = TF_H; /* ... its size */
static uint32_t g_fixed_w;                      /* the fixed 2x filter: its input's width */

/* the disc of the picture at size w x h (its radius 0.3 of the height) with a ramp behind */
static void tf_disc(uint32_t w, uint32_t h, uint16_t *out)
{
    uint32_t x, y, sx, sy, inside;
    uint8_t c;
    double cx, cy, r, px, py, ramp;

    cx = (double)w * 0.45;
    cy = (double)h * 0.5;
    r = (double)h * 0.3;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            inside = 0;

            for (sy = 0; sy < TF_SUPER; sy++) {
                for (sx = 0; sx < TF_SUPER; sx++) {
                    px = (double)x + ((double)sx + 0.5) / TF_SUPER - cx;
                    py = (double)y + ((double)sy + 0.5) / TF_SUPER - cy;
                    inside += (px * px + py * py < r * r) ? 1u : 0u;
                }
            }

            ramp = 0.15 + 0.3 * (double)x / (double)w;

            for (c = 0; c < 3u; c++) {
                out[((size_t)y * w + x) * 3u + c] =
                    (uint16_t)((ramp * (1.0 - (double)inside / (TF_SUPER * TF_SUPER)) +
                                (0.9 - 0.2 * c) * (double)inside / (TF_SUPER * TF_SUPER)) *
                                   65535.0 +
                               0.5);
            }
        }
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL tf_fixed_create(hdrg_filter_t *filter,
                                                      const hdrg_options_t *options, void **inst,
                                                      const hdrg_plugin_api_t *api)
{
    (void)filter;
    (void)options;
    (void)api;
    *inst = NULL;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL tf_fixed_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;
}

/* the fixed 2x filter (a model's factor): twice the input, whatever the size asked for */
static hdrg_status_t HDRG_PLUGIN_CALL tf_fixed_configure(void *inst, hdrg_negotiation_t *nego,
                                                         const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;
    uint32_t height, format;

    (void)inst;

    status = api->nego_input(nego, &g_fixed_w, &height, &format);

    return (status == HDRG_OK)
               ? api->nego_set_output(nego, g_fixed_w * 2u, height * 2u, HDRG_FORMAT_RGB16)
               : status;
}

/* ... each pixel twice each way (whole frames) */
static hdrg_status_t HDRG_PLUGIN_CALL tf_fixed_process(void *inst, hdrg_job_t *job,
                                                       const hdrg_plugin_api_t *api)
{
    const uint16_t *row;
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, x;
    uint8_t *out, c;
    size_t in_stride, out_stride;

    (void)inst;

    status = api->job_rows(job, &begin, &end);
    status =
        (status == HDRG_OK) ? api->job_input(job, 0, &in, &in_stride, &in_begin, &in_end) : status;
    status = (status == HDRG_OK) ? api->job_output(job, 0, &out, &out_stride) : status;

    for (y = begin; status == HDRG_OK && y < end; y++) {
        row = (const uint16_t *)(const void *)(in + (size_t)(y / 2u - in_begin) * in_stride);

        for (x = 0; x < g_fixed_w * 2u; x++) {
            for (c = 0; c < 3u; c++) {
                ((uint16_t *)(void *)(out + (size_t)(y - begin) * out_stride))[x * 3u + c] =
                    row[(x / 2u) * 3u + c];
            }
        }
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL tf_fixed_init(hdrg_plugin_t *plugin,
                                                    const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, "org.test.fixed", 14, "Fixed factor", 12, "1.0.0", 5,
                                  "test", 4);
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, "MIT", 3, "test", 4) : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    cls = (status == HDRG_OK) ? api->class_create(plugin, "org.test.fixed2x", 16) : NULL;
    status = (cls != NULL) ? api->class_set_stages(cls, HDRG_STAGE_SCALE) : HDRG_ERR_PLUGIN;
    status = (status == HDRG_OK)
                 ? api->class_set_flags(cls, HDRG_FILTER_PURE | HDRG_FILTER_DETERMINISTIC)
                 : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, tf_fixed_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, tf_fixed_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, tf_fixed_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, tf_fixed_process) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

/* the SCALE stage of id (option: "key=value", NULL for none) over pixels to w x h in bands */
static hdrg_status_t tf_scale(const char *id, const char *option, const uint16_t *pixels,
                              uint32_t w, uint32_t h, uint32_t band, uint32_t threads,
                              uint16_t *out)
{
    const hdrg_class_t *cls;
    const char *at, *next;
    hdrg_filter_entry_t entry;
    hdrg_raw_image_t raw;
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_stage_t scale;
    hdrg_exec_t exec;
    hdrg_options_t *text;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t y0, rows;
    size_t len;
    char key[32], value[32];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, tf_fixed_init));
    memset(&exec, 0, sizeof(exec));

    if (threads > 1u) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_pool_create(&exec.pool, &g_alloc, threads - 1u, NULL, NULL));
        exec.max_workers = threads;
    }

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)pixels;
    raw.width = g_src_w;
    raw.height = g_src_h;
    raw.bit_depth = 16;
    raw.stride = (size_t)g_src_w * 6u;
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, &src));

    cls = hdrg_host_find_class(&ctx->host, id, strlen(id));
    TEST_ASSERT_NOT_NULL(cls);
    text = NULL;

    /* options: "key=value", several separated by ':' */
    for (at = option; at != NULL && *at != '\0'; at = (next != NULL) ? next + 1 : NULL) {
        next = strchr(at, ':');
        TEST_ASSERT_TRUE(strchr(at, '=') != NULL && (size_t)(strchr(at, '=') - at) < sizeof(key));
        memcpy(key, at, (size_t)(strchr(at, '=') - at));
        key[strchr(at, '=') - at] = '\0';
        len = (next != NULL) ? (size_t)(next - strchr(at, '=') - 1) : strlen(strchr(at, '=') + 1);
        TEST_ASSERT_TRUE(len < sizeof(value));
        memcpy(value, strchr(at, '=') + 1, len);
        value[len] = '\0';

        if (text == NULL) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &text));
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(text, key, value));
    }

    entry.cls = cls;
    entry.stage = HDRG_STAGE_SCALE;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_host_resolve_options(&ctx->host, cls, text, &entry.options));
    hdrg_options_destroy(text);

    status = hdrg_test_stage_open(&g_alloc, &entry, &src, w, h, band, NULL, &ctx->host, &scale);

    for (y0 = 0; status == HDRG_OK && y0 < h; y0 += rows) {
        rows = (h - y0 < band) ? h - y0 : band;
        status = hdrg_stage_rows(&scale, (threads > 1u) ? &exec : NULL, y0, rows,
                                 out + (size_t)w * 3u * y0, &ctx->host);
    }

    if (status == HDRG_OK) {
        hdrg_stage_close(&scale);
    }

    hdrg_options_destroy(entry.options);
    hdrg_source_free(&g_alloc, &src);

    if (exec.pool != NULL) {
        hdrg_pool_destroy(exec.pool);
    }

    hdrg_context_destroy(ctx);

    return status;
}

/*
 * n frames (packed RGB16 of w x h at frames) through a session (STATIC: a
 * fixed mapping of each pixel, no PREFILTER stage, threads) to out_w x
 * out_h with the SCALE filter id and options (NULL: none, the input's
 * size) into out (packed 16-bit PQ R'G'B'); the frames received
 */
static uint32_t tf_session(const char *id, const char *options, const uint16_t *frames, uint32_t w,
                           uint32_t h, uint32_t n, uint32_t out_w, uint32_t out_h, uint32_t threads,
                           uint16_t *out)
{
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_session_frame_t frame;
    hdrg_session_output_t output;
    hdrg_options_t *text;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t sent, received;
    char key[32];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, tf_fixed_init));

    if (id != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &text));

        if (options != NULL) {
            snprintf(key, sizeof(key), "%s", options);
            key[strcspn(key, "=")] = '\0';
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(text, key, strchr(options, '=') + 1));
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, id, text));
        hdrg_options_destroy(text);
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.grain_policy = HDRG_GRAIN_PRESERVE;
    params.deblock_strength = 0.0f;
    params.deband_strength = 0.0f;
    params.mctf_strength = 0.0f;
    memset(&format, 0, sizeof(format));
    format.width = w;
    format.height = h;
    format.bit_depth = 16;
    format.stride = (size_t)w * 6u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_session_config_init(&config);
    config.output.bits = 16;
    config.dither_off = true;
    config.flash_off = true;
    config.temporal = HDRG_TEMPORAL_STATIC;
    config.rate_num = 30;
    config.rate_den = 1;
    config.output_width = (id != NULL) ? out_w : 0u;
    config.output_height = (id != NULL) ? out_h : 0u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &config, &session));
    sent = 0;
    received = 0;

    for (;;) {
        memset(&output, 0, sizeof(output));
        output.planes[0] = (uint8_t *)(out + (size_t)received * out_w * out_h * 3u);
        output.strides[0] = (size_t)out_w * 6u;

        status = hdrg_session_receive_frame(session, &output);
        if (status == HDRG_ERR_AGAIN) {
            memset(&frame, 0, sizeof(frame));
            frame.planes[0] = (const uint8_t *)(frames + (size_t)sent * w * h * 3u);
            frame.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < n) ? &frame : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    return received;
}

/*
 * three of the same frames through a session with the SCALE filter id to sx x sy the size:
 * the same bytes each time (what a filter keeps from frame to frame reused alike), out (the
 * frames, freed by the caller) not flat
 */
static uint16_t *tf_frames_alike(const char *id, const char *options, uint32_t sx, uint32_t sy)
{
    const size_t large = (size_t)TF_W * sx * TF_H * sy * 3u;
    uint32_t k;
    uint16_t *frames, *out;
    size_t i, changes;

    frames = (uint16_t *)malloc((size_t)TF_W * TF_H * 3u * 3u * sizeof(uint16_t));
    out = (uint16_t *)malloc(large * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(frames != NULL && out != NULL);

    for (k = 0; k < 3u; k++) {
        memcpy(frames + (size_t)k * TF_W * TF_H * 3u, g_pixels,
               (size_t)TF_W * TF_H * 3u * sizeof(uint16_t));
    }

    TEST_ASSERT_EQUAL_UINT32(
        3, tf_session(id, options, frames, TF_W, TF_H, 3, TF_W * sx, TF_H * sy, 1, out));
    TEST_ASSERT_EQUAL_MEMORY(out, out + large, large * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_MEMORY(out, out + 2u * large, large * sizeof(uint16_t));
    changes = 0;

    for (i = 3; i < large; i++) {
        changes += (out[i] != out[i - 3u]) ? 1u : 0u;
    }

    TEST_ASSERT_TRUE(changes > large / 100u);
    free(frames);

    return out;
}

/* whether the build has the filter class id */
static bool tf_has(const char *id)
{
    hdrg_context_t *ctx;
    bool has;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    has = hdrg_host_find_class(&ctx->host, id, strlen(id)) != NULL;
    hdrg_context_destroy(ctx);

    return has;
}

/* the PSNR of a against b (n samples) in dB */
static double tf_psnr(const uint16_t *a, const uint16_t *b, size_t n)
{
    size_t i;
    double sum, d;

    sum = 0.0;

    for (i = 0; i < n; i++) {
        d = (double)a[i] - (double)b[i];
        sum += d * d;
    }

    return 10.0 * log10(65535.0 * 65535.0 / (sum / (double)n + 1e-9));
}

static void test_fsr_bands(void)
{
    static const uint32_t sizes[3][2] = {{144, 96}, {192, 128}, {227, 152}}, bands[3] = {7, 16, 33};
    static const char *const sharpen[2] = {"sharpen=0", "sharpen=1"};
    uint32_t s, b, k;
    uint16_t *reference, *banded;
    size_t n;

    HDRG_TEST_REQUIRE_THREADS();

    for (k = 0; k < 2u; k++) {
        for (s = 0; s < 3u; s++) {
            n = (size_t)sizes[s][0] * sizes[s][1] * 3u;
            reference = (uint16_t *)malloc(n * sizeof(uint16_t));
            banded = (uint16_t *)malloc(n * sizeof(uint16_t));
            TEST_ASSERT_NOT_NULL(reference);
            TEST_ASSERT_NOT_NULL(banded);
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  tf_scale("hdrg.scale.fsr1", sharpen[k], g_pixels, sizes[s][0],
                                           sizes[s][1], sizes[s][1], 1, reference));

            for (b = 0; b < 3u; b++) {
                TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                      tf_scale("hdrg.scale.fsr1", sharpen[k], g_pixels, sizes[s][0],
                                               sizes[s][1], bands[b], 6, banded));
                TEST_ASSERT_EQUAL_MEMORY(reference, banded, n * sizeof(uint16_t));
            }

            free(reference);
            free(banded);
        }
    }
}

static void test_fsr_flat(void)
{
    uint32_t worst;
    uint16_t *flat, *out;
    size_t i, n;

    n = (size_t)200 * 150 * 3u;
    flat = (uint16_t *)malloc((size_t)TF_W * TF_H * 3u * sizeof(uint16_t));
    out = (uint16_t *)malloc(n * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(flat);
    TEST_ASSERT_NOT_NULL(out);

    for (i = 0; i < (size_t)TF_W * TF_H * 3u; i++) {
        flat[i] = (uint16_t)(20000u + (i % 3u) * 7000u);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tf_scale("hdrg.scale.fsr1", "sharpen=0", flat, 200, 150, 150, 1, out));

    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16(flat[i % 3u], out[i]);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tf_scale("hdrg.scale.fsr1", "sharpen=1", flat, 200, 150, 150, 1, out));
    worst = 0;

    for (i = 0; i < n; i++) {
        worst = HDRG_MAX(worst, (uint32_t)abs((int)out[i] - (int)flat[i % 3u]));
    }

    TEST_ASSERT_TRUE(worst <= 1u);

    free(flat);
    free(out);
}

static void test_fsr_sizes(void)
{
    uint16_t *out, lo, hi;
    size_t i, n;

    n = (size_t)TF_W * TF_H * 3u;
    out = (uint16_t *)malloc((size_t)300 * 200 * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(out);

    /* a smaller output is not FSR's; the same size is the source */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN,
                          tf_scale("hdrg.scale.fsr1", NULL, g_pixels, 80, 64, 64, 1, out));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tf_scale("hdrg.scale.fsr1", NULL, g_pixels, TF_W, TF_H, TF_H, 1, out));
    TEST_ASSERT_EQUAL_MEMORY(g_pixels, out, n * sizeof(uint16_t));

    /* EASU within the source's range */
    lo = 65535;
    hi = 0;

    for (i = 0; i < n; i++) {
        lo = (g_pixels[i] < lo) ? g_pixels[i] : lo;
        hi = (g_pixels[i] > hi) ? g_pixels[i] : hi;
    }

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tf_scale("hdrg.scale.fsr1", "sharpen=0", g_pixels, 300, 200, 200, 1, out));

    for (i = 0; i < (size_t)300 * 200 * 3u; i++) {
        TEST_ASSERT_TRUE(out[i] >= lo && out[i] <= hi);
    }

    free(out);
}

/* the disc upscaled twice against the disc drawn at that size; RCAS steepens its edge */
static void test_fsr_disc(void)
{
    const size_t n = (size_t)TF_W * TF_H * 12u;
    uint32_t x, y;
    uint16_t *truth, *fsr, *sharp, *bilinear, *lanczos, *nis, *edge;
    size_t i;
    char line[160];
    double plain_edge, sharp_edge;

    truth = (uint16_t *)malloc(n * sizeof(uint16_t));
    fsr = (uint16_t *)malloc(n * sizeof(uint16_t));
    sharp = (uint16_t *)malloc(n * sizeof(uint16_t));
    bilinear = (uint16_t *)malloc(n * sizeof(uint16_t));
    lanczos = (uint16_t *)malloc(n * sizeof(uint16_t));
    nis = (uint16_t *)malloc(n * sizeof(uint16_t));
    edge = (uint16_t *)malloc(n * sizeof(uint16_t));
    TEST_ASSERT_TRUE(truth != NULL && fsr != NULL && sharp != NULL && bilinear != NULL &&
                     lanczos != NULL && nis != NULL && edge != NULL);
    tf_disc(TF_W * 2u, TF_H * 2u, truth);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.fsr1", "sharpen=0", g_pixels, TF_W * 2u,
                                            TF_H * 2u, TF_H * 2u, 1, fsr));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.fsr1", "sharpen=1", g_pixels, TF_W * 2u,
                                            TF_H * 2u, TF_H * 2u, 1, sharp));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.bilinear", NULL, g_pixels, TF_W * 2u,
                                            TF_H * 2u, TF_H * 2u, 1, bilinear));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.edge", NULL, g_pixels, TF_W * 2u, TF_H * 2u,
                                            TF_H * 2u, 1, edge));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.nis", NULL, g_pixels, TF_W * 2u, TF_H * 2u,
                                            TF_H * 2u, 1, nis));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.lanczos", NULL, g_pixels, TF_W * 2u,
                                            TF_H * 2u, TF_H * 2u, 1, lanczos));

    snprintf(line, sizeof(line),
             "the disc at 2x: edge %.2f dB, FSR %.2f, FSR + RCAS %.2f, NIS %.2f, Lanczos %.2f, "
             "bilinear %.2f",
             tf_psnr(edge, truth, n), tf_psnr(fsr, truth, n), tf_psnr(sharp, truth, n),
             tf_psnr(nis, truth, n), tf_psnr(lanczos, truth, n), tf_psnr(bilinear, truth, n));
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(tf_psnr(fsr, truth, n) > tf_psnr(bilinear, truth, n));
    TEST_ASSERT_TRUE(tf_psnr(nis, truth, n) > tf_psnr(bilinear, truth, n));

    /* the edge's steepness: the summed absolute steps along the rows (G) */
    plain_edge = 0.0;
    sharp_edge = 0.0;

    for (y = 0; y < TF_H * 2u; y++) {
        for (x = 1; x < TF_W * 2u; x++) {
            i = ((size_t)y * TF_W * 2u + x) * 3u + 1u;
            plain_edge += fabs((double)fsr[i] - (double)fsr[i - 3u]);
            sharp_edge += fabs((double)sharp[i] - (double)sharp[i - 3u]);
        }
    }

    TEST_ASSERT_TRUE(sharp_edge > plain_edge);

    free(truth);
    free(fsr);
    free(sharp);
    free(bilinear);
    free(lanczos);
    free(nis);
    free(edge);
}

/*
 * hdrg.scale.nis: any band split and thread count give the same output; a
 * flat picture stays within 8 codes (NIS's tables' rows sum to 1 within
 * their 4 digits: 5 measured); up to twice the size (more or less refused,
 * the same size the source)
 */
static void test_nis(void)
{
    static const uint32_t sizes[3][2] = {{144, 96}, {192, 128}, {170, 113}}, bands[3] = {7, 16, 33};
    static const char *const sharpness[2] = {"sharpness=0.5", "sharpness=0"};
    uint32_t s, b, k, worst;
    uint16_t *reference, *banded, *flat;
    size_t i, n;

    HDRG_TEST_REQUIRE_THREADS();

    reference = (uint16_t *)malloc((size_t)192 * 128 * 3u * sizeof(uint16_t));
    banded = (uint16_t *)malloc((size_t)192 * 128 * 3u * sizeof(uint16_t));
    flat = (uint16_t *)malloc((size_t)TF_W * TF_H * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(reference != NULL && banded != NULL && flat != NULL);

    for (k = 0; k < 2u; k++) {
        for (s = 0; s < 3u; s++) {
            n = (size_t)sizes[s][0] * sizes[s][1] * 3u;
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  tf_scale("hdrg.scale.nis", sharpness[k], g_pixels, sizes[s][0],
                                           sizes[s][1], sizes[s][1], 1, reference));

            for (b = 0; b < 3u; b++) {
                TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                      tf_scale("hdrg.scale.nis", sharpness[k], g_pixels,
                                               sizes[s][0], sizes[s][1], bands[b], 6, banded));
                TEST_ASSERT_EQUAL_MEMORY(reference, banded, n * sizeof(uint16_t));
            }
        }
    }

    for (i = 0; i < (size_t)TF_W * TF_H * 3u; i++) {
        flat[i] = (uint16_t)(20000u + (i % 3u) * 7000u);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tf_scale("hdrg.scale.nis", NULL, flat, 170, 113, 113, 1, reference));
    worst = 0;

    for (i = 0; i < (size_t)170 * 113 * 3u; i++) {
        worst = HDRG_MAX(worst, (uint32_t)abs((int)reference[i] - (int)flat[i % 3u]));
    }

    TEST_ASSERT_TRUE(worst <= 8u);

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN,
                          tf_scale("hdrg.scale.nis", NULL, g_pixels, 200, 130, 130, 1, reference));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN,
                          tf_scale("hdrg.scale.nis", NULL, g_pixels, 80, 64, 64, 1, reference));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tf_scale("hdrg.scale.nis", NULL, g_pixels, TF_W, TF_H, TF_H, 1, reference));
    TEST_ASSERT_EQUAL_MEMORY(g_pixels, reference, (size_t)TF_W * TF_H * 3u * sizeof(uint16_t));

    free(reference);
    free(banded);
    free(flat);
}

/*
 * hdrg.scale.edge: any band split and thread count give the same output; a
 * flat picture stays flat exactly and the output within the source's range
 * (each pixel within its four nearest); a smaller output refused, the same
 * size the source
 */
static void test_edge(void)
{
    static const uint32_t sizes[3][2] = {{144, 96}, {192, 128}, {227, 152}}, bands[3] = {7, 16, 33};
    uint32_t s, b;
    uint16_t *reference, *banded, *flat, lo, hi;
    size_t i, n;

    HDRG_TEST_REQUIRE_THREADS();

    reference = (uint16_t *)malloc((size_t)227 * 152 * 3u * sizeof(uint16_t));
    banded = (uint16_t *)malloc((size_t)227 * 152 * 3u * sizeof(uint16_t));
    flat = (uint16_t *)malloc((size_t)TF_W * TF_H * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(reference != NULL && banded != NULL && flat != NULL);

    for (s = 0; s < 3u; s++) {
        n = (size_t)sizes[s][0] * sizes[s][1] * 3u;
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.edge", NULL, g_pixels, sizes[s][0],
                                                sizes[s][1], sizes[s][1], 1, reference));

        for (b = 0; b < 3u; b++) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.edge", NULL, g_pixels, sizes[s][0],
                                                    sizes[s][1], bands[b], 6, banded));
            TEST_ASSERT_EQUAL_MEMORY(reference, banded, n * sizeof(uint16_t));
        }
    }

    lo = 65535;
    hi = 0;

    for (i = 0; i < (size_t)TF_W * TF_H * 3u; i++) {
        lo = (g_pixels[i] < lo) ? g_pixels[i] : lo;
        hi = (g_pixels[i] > hi) ? g_pixels[i] : hi;
        flat[i] = (uint16_t)(20000u + (i % 3u) * 7000u);
    }

    for (i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(reference[i] >= lo && reference[i] <= hi);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tf_scale("hdrg.scale.edge", NULL, flat, 227, 152, 152, 1, reference));

    for (i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16(flat[i % 3u], reference[i]);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN,
                          tf_scale("hdrg.scale.edge", NULL, g_pixels, 80, 64, 64, 1, reference));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, tf_scale("hdrg.scale.edge", NULL, g_pixels, TF_W, TF_H, TF_H, 1, reference));
    TEST_ASSERT_EQUAL_MEMORY(g_pixels, reference, (size_t)TF_W * TF_H * 3u * sizeof(uint16_t));

    free(reference);
    free(banded);
    free(flat);
}

/* kodim23 as packed RGB16 (its 8-bit codes x 257), *w x *h even (malloc's) */
static uint16_t *tf_kodim23(uint32_t *w, uint32_t *h)
{
    hdrg_decoded_t decoded;
    FILE *fp;
    uint32_t x, y;
    uint16_t *full;
    uint8_t *data;
    long length;
    size_t size;

    fp = fopen(HDRG_TEST_ASSETS_DIR "/kodim23.png", "rb");
    TEST_ASSERT_NOT_NULL(fp);
    TEST_ASSERT_EQUAL_INT(0, fseek(fp, 0, SEEK_END));
    length = ftell(fp);
    TEST_ASSERT_TRUE(length > 0);
    rewind(fp);
    size = (size_t)length;
    data = (uint8_t *)malloc(size);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)size, (uint32_t)fread(data, 1, size, fp));
    fclose(fp);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, data, size, UINT64_C(1) << 26,
                                                   HDRG_CHROMA_FIT_LINEAR, &decoded));
    free(data);
    TEST_ASSERT_EQUAL_UINT32(8, decoded.bit_depth);
    *w = decoded.width & ~1u;
    *h = decoded.height & ~1u;
    full = (uint16_t *)malloc((size_t)*w * *h * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(full);

    for (y = 0; y < *h; y++) {
        for (x = 0; x < *w * 3u; x++) {
            full[(size_t)y * *w * 3u + x] =
                (uint16_t)(decoded.pixels[(size_t)y * decoded.stride + x] * 257u);
        }
    }

    hdrg_mem_free(&g_alloc, decoded.pixels);
    hdrg_mem_free(&g_alloc, decoded.icc);
    hdrg_mem_free(&g_alloc, decoded.exif);

    return full;
}

/*
 * a photograph (kodim23, 768 x 512) averaged down to half and upscaled back
 * by each scaler, against itself: the PSNR of each (reported)
 */
static void test_photo(void)
{
    static const char *const ids[8] = {"hdrg.scale.edge",    "hdrg.scale.fsr1",
                                       "hdrg.scale.fsr1",    "hdrg.scale.nis",
                                       "hdrg.scale.nis",     "hdrg.scale.lanczos",
                                       "hdrg.scale.bicubic", "hdrg.scale.bilinear"},
                             *const options[8] = {
                                 NULL,          "sharpen=0", "sharpen=1", "sharpness=0.5",
                                 "sharpness=0", NULL,        NULL,        NULL};
    uint32_t w, h, x, y, k;
    uint16_t *full, *half, *out;
    uint8_t c;
    char line[160];

    full = tf_kodim23(&w, &h);
    half = (uint16_t *)malloc((size_t)w * h * 3u / 4u * sizeof(uint16_t));
    out = (uint16_t *)malloc((size_t)w * h * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(half != NULL && out != NULL);

    for (y = 0; y < h / 2u; y++) {
        for (x = 0; x < w / 2u; x++) {
            for (c = 0; c < 3u; c++) {
                half[((size_t)y * (w / 2u) + x) * 3u + c] =
                    (uint16_t)(((uint32_t)full[((size_t)(2u * y) * w + 2u * x) * 3u + c] +
                                full[((size_t)(2u * y) * w + 2u * x + 1u) * 3u + c] +
                                full[((size_t)(2u * y + 1u) * w + 2u * x) * 3u + c] +
                                full[((size_t)(2u * y + 1u) * w + 2u * x + 1u) * 3u + c] + 2u) /
                               4u);
            }
        }
    }

    g_src_w = w / 2u;
    g_src_h = h / 2u;

    for (k = 0; k < 8u; k++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale(ids[k], options[k], half, w, h, h, 1, out));
        snprintf(line, sizeof(line), "kodim23 at half, back to %ux%u: %s (%s) %.2f dB", w, h,
                 ids[k], (options[k] != NULL) ? options[k] : "default",
                 tf_psnr(out, full, (size_t)w * h * 3u));
        TEST_MESSAGE(line);
    }

    g_src_w = TF_W;
    g_src_h = TF_H;
    free(full);
    free(half);
    free(out);
}

/*
 * A SCALE filter of a fixed factor (2x, whole frames: a model's) asked for
 * another size: its output fitted by the host's Lanczos, the same as its 2x
 * then Lanczos from that size, larger or smaller, whatever the bands and
 * threads
 */
static void test_fixed(void)
{
    static const uint32_t sizes[2][2] = {{TF_W * 3u, TF_H * 3u}, {TF_W * 3u / 2u, TF_H * 3u / 2u}};
    uint32_t s;
    uint16_t *twice, *got, *want;
    size_t n;

    twice = (uint16_t *)malloc((size_t)TF_W * TF_H * 4u * 3u * sizeof(uint16_t));
    got = (uint16_t *)malloc((size_t)TF_W * TF_H * 9u * 3u * sizeof(uint16_t));
    want = (uint16_t *)malloc((size_t)TF_W * TF_H * 9u * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(twice != NULL && got != NULL && want != NULL);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("org.test.fixed2x", NULL, g_pixels, TF_W * 2u,
                                            TF_H * 2u, TF_H * 2u, 1, twice));
    TEST_ASSERT_EQUAL_UINT16(g_pixels[0], twice[3]);

    for (s = 0; s < 2u; s++) {
        n = (size_t)sizes[s][0] * sizes[s][1] * 3u;
        g_src_w = TF_W * 2u;
        g_src_h = TF_H * 2u;
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.lanczos", NULL, twice, sizes[s][0],
                                                sizes[s][1], sizes[s][1], 1, want));
        g_src_w = TF_W;
        g_src_h = TF_H;
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("org.test.fixed2x", NULL, g_pixels, sizes[s][0],
                                                sizes[s][1], sizes[s][1], 1, got));
        TEST_ASSERT_EQUAL_MEMORY(want, got, n * sizeof(uint16_t));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("org.test.fixed2x", NULL, g_pixels, sizes[s][0],
                                                sizes[s][1], 7, HDRG_TEST_THREADS(4u), got));
        TEST_ASSERT_EQUAL_MEMORY(want, got, n * sizeof(uint16_t));
    }

    free(twice);
    free(got);
    free(want);

    /* a session's frames: the fitting stage made once, the same frames the same bytes */
    free(tf_frames_alike("org.test.fixed2x", NULL, 3, 2));
}

#if HDRG_BUILTIN_SCALE_TEMPORAL
/*
 * the moving texture at (x, y) of the large frames at time t: 24 waves of
 * scattered directions and frequencies (0.04 to 0.36 cycles a pixel: some
 * past the small frames' limit, 0.25), no period of its own
 */
static double tf_pattern(double x, double y, double t)
{
    const double u = x - 0.73 * t, v = y - 0.41 * t;
    uint32_t k, seed;
    double sum, angle, frequency, phase;

    sum = 0.5;
    seed = 12345u;

    for (k = 0; k < 24u; k++) {
        seed = seed * 1664525u + 1013904223u;
        angle = (double)(seed >> 8) / 16777216.0 * 3.14159265358979;
        seed = seed * 1664525u + 1013904223u;
        frequency = 0.04 + 0.32 * (double)(seed >> 8) / 16777216.0;
        seed = seed * 1664525u + 1013904223u;
        phase = (double)(seed >> 8) / 16777216.0 * 6.28318530717959;
        sum +=
            0.025 * sin(6.28318530717959 * frequency * (u * cos(angle) + v * sin(angle)) + phase);
    }

    return sum;
}

/* the PSNR of a and b (frames of w x h) within the margin from the edges */
static double tf_psnr_inner(const uint16_t *a, const uint16_t *b, uint32_t w, uint32_t h,
                            uint32_t margin)
{
    uint32_t x, y, c;
    size_t n, i;
    double sum, d;

    sum = 0.0;
    n = 0;

    for (y = margin; y < h - margin; y++) {
        for (x = margin; x < w - margin; x++) {
            for (c = 0; c < 3u; c++) {
                i = ((size_t)y * w + x) * 3u + c;
                d = (double)a[i] - (double)b[i];
                sum += d * d;
                n++;
            }
        }
    }

    return 10.0 * log10(65535.0 * 65535.0 / (sum / (double)n + 1e-9));
}

/*
 * hdrg.scale.temporal (the multi-frame upscale): a still (no window) is the
 * edge-adaptive upscale's bytes; a session of a moving texture sampled at
 * half the size (aliased) comes nearer the texture drawn at the full size
 * than the edge-adaptive upscale of each frame alone (1 dB at least), the
 * same whatever the threads; a photograph panned (kodim23, averaged 2 x 2:
 * little aliasing) no further from it
 */
static void test_temporal(void)
{
    const uint32_t w = 96, h = 64, n = 7;
    const size_t small = (size_t)w * h * 3u, large = small * 4u;
    uint32_t x, y, t, c, photo_w, photo_h;
    uint16_t *a, *b, *frames, *truth, *out, *edge, *threads, *photo;
    char line[128];
    double p_edge, p_temporal;

    a = (uint16_t *)malloc((size_t)TF_W * TF_H * 4u * 3u * sizeof(uint16_t));
    b = (uint16_t *)malloc((size_t)TF_W * TF_H * 4u * 3u * sizeof(uint16_t));
    frames = (uint16_t *)malloc(small * n * sizeof(uint16_t));
    truth = (uint16_t *)malloc(large * n * sizeof(uint16_t));
    out = (uint16_t *)malloc(large * n * sizeof(uint16_t));
    edge = (uint16_t *)malloc(large * n * sizeof(uint16_t));
    threads = (uint16_t *)malloc(large * n * sizeof(uint16_t));
    TEST_ASSERT_TRUE(a != NULL && b != NULL && frames != NULL && truth != NULL && out != NULL &&
                     edge != NULL && threads != NULL);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.temporal", NULL, g_pixels, TF_W * 2u,
                                            TF_H * 2u, TF_H * 2u, 1, a));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale("hdrg.scale.edge", NULL, g_pixels, TF_W * 2u, TF_H * 2u,
                                            TF_H * 2u, 1, b));
    TEST_ASSERT_EQUAL_MEMORY(b, a, (size_t)TF_W * TF_H * 4u * 3u * sizeof(uint16_t));

    /* the same size: the source; upscaling only (a smaller side refused) */
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          tf_scale("hdrg.scale.temporal", NULL, g_pixels, TF_W, TF_H, TF_H, 1, a));
    TEST_ASSERT_EQUAL_MEMORY(g_pixels, a, (size_t)TF_W * TF_H * 3u * sizeof(uint16_t));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, tf_scale("hdrg.scale.temporal", NULL, g_pixels,
                                                    TF_W / 2u, TF_H / 2u, TF_H / 2u, 1, a));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, tf_scale("hdrg.scale.temporal", NULL, g_pixels,
                                                    TF_W * 2u, TF_H / 2u, TF_H / 2u, 1, a));

    for (t = 0; t < n; t++) {
        for (y = 0; y < h * 2u; y++) {
            for (x = 0; x < w * 2u; x++) {
                for (c = 0; c < 3u; c++) {
                    truth[t * large + ((size_t)y * w * 2u + x) * 3u + c] =
                        (uint16_t)(tf_pattern((double)x, (double)y, (double)t) * (0.7 + 0.15 * c) *
                                       65535.0 +
                                   0.5);
                }
            }
        }

        /* the small frames: the pattern at their pixels' centers (2 x + 0.5 of the large's) */
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                for (c = 0; c < 3u; c++) {
                    frames[t * small + ((size_t)y * w + x) * 3u + c] =
                        (uint16_t)(tf_pattern(2.0 * x + 0.5, 2.0 * y + 0.5, (double)t) *
                                       (0.7 + 0.15 * c) * 65535.0 +
                                   0.5);
                }
            }
        }
    }

    TEST_ASSERT_EQUAL_UINT32(
        n, tf_session(NULL, NULL, truth, w * 2u, h * 2u, n, w * 2u, h * 2u, 1, truth));
    TEST_ASSERT_EQUAL_UINT32(
        n, tf_session("hdrg.scale.edge", NULL, frames, w, h, n, w * 2u, h * 2u, 1, edge));
    TEST_ASSERT_EQUAL_UINT32(
        n, tf_session("hdrg.scale.temporal", NULL, frames, w, h, n, w * 2u, h * 2u, 1, out));
    TEST_ASSERT_EQUAL_UINT32(
        n, tf_session("hdrg.scale.temporal", NULL, frames, w, h, n, w * 2u, h * 2u, 4, threads));
    TEST_ASSERT_EQUAL_MEMORY(out, threads, large * n * sizeof(uint16_t));

    p_edge = tf_psnr_inner(edge + 3u * large, truth + 3u * large, w * 2u, h * 2u, 16);
    p_temporal = tf_psnr_inner(out + 3u * large, truth + 3u * large, w * 2u, h * 2u, 16);
    snprintf(line, sizeof(line), "aliased texture at 2x, frame 3: edge %.2f dB, temporal %.2f dB",
             p_edge, p_temporal);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(p_temporal > p_edge + 1.0);

    /* the photograph: a crop moving a large pixel a frame, averaged to half */
    photo = tf_kodim23(&photo_w, &photo_h);

    for (t = 0; t < n; t++) {
        for (y = 0; y < h * 2u; y++) {
            for (x = 0; x < w * 2u * 3u; x++) {
                truth[t * large + (size_t)y * w * 6u + x] =
                    photo[((size_t)(y + 120u) * photo_w + 200u + t) * 3u + x];
            }
        }

        for (y = 0; y < h; y++) {
            for (x = 0; x < w * 3u; x++) {
                c = x % 3u;
                frames[t * small + (size_t)y * w * 3u + x] =
                    (uint16_t)(((uint32_t)
                                    truth[t * large +
                                          ((size_t)(2u * y) * w * 2u + 2u * (x / 3u)) * 3u + c] +
                                truth[t * large +
                                      ((size_t)(2u * y) * w * 2u + 2u * (x / 3u) + 1u) * 3u + c] +
                                truth[t * large +
                                      ((size_t)(2u * y + 1u) * w * 2u + 2u * (x / 3u)) * 3u + c] +
                                truth[t * large +
                                      ((size_t)(2u * y + 1u) * w * 2u + 2u * (x / 3u) + 1u) * 3u +
                                      c] +
                                2u) /
                               4u);
            }
        }
    }

    free(photo);
    TEST_ASSERT_EQUAL_UINT32(
        n, tf_session(NULL, NULL, truth, w * 2u, h * 2u, n, w * 2u, h * 2u, 1, truth));
    TEST_ASSERT_EQUAL_UINT32(
        n, tf_session("hdrg.scale.edge", NULL, frames, w, h, n, w * 2u, h * 2u, 1, edge));
    TEST_ASSERT_EQUAL_UINT32(
        n, tf_session("hdrg.scale.temporal", NULL, frames, w, h, n, w * 2u, h * 2u, 1, out));
    p_edge = tf_psnr_inner(edge + 3u * large, truth + 3u * large, w * 2u, h * 2u, 16);
    p_temporal = tf_psnr_inner(out + 3u * large, truth + 3u * large, w * 2u, h * 2u, 16);
    snprintf(line, sizeof(line), "kodim23 panned at 2x, frame 3: edge %.2f dB, temporal %.2f dB",
             p_edge, p_temporal);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(p_temporal > p_edge - 0.1);

    free(a);
    free(b);
    free(frames);
    free(truth);
    free(out);
    free(edge);
    free(threads);
}
#endif

/*
 * the source converted (the default gain map: no lcms2 needed) to w x h
 * with the preset and upscaler (0xff: the preset's) and the SCALE filter id
 * added (NULL: none)
 */
static void tf_convert(uint8_t preset, uint8_t upscaler, const char *id, uint32_t w, uint32_t h,
                       uint8_t **out, size_t *size)
{
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 1));

    if (id != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, id, NULL));
    }

    hdrg_params_init(&params, preset);

    if (upscaler != 0xffu) {
        params.upscaler = upscaler;
    }

    hdrg_encode_params_init(&encode);
    encode.output_width = w;
    encode.output_height = h;
    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TF_W;
    raw.height = TF_H;
    raw.bit_depth = 16;
    raw.stride = (size_t)TF_W * 6u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, out, size));
    hdrg_context_destroy(ctx);
}

/* whether the two conversions' files are the same bytes (both freed) */
static bool tf_same(uint8_t *a, size_t a_size, uint8_t *b, size_t b_size)
{
    bool same;

    same = a_size == b_size && memcmp(a, b, a_size) == 0;
    hdrg_free(a);
    hdrg_free(b);

    return same;
}

/*
 * the built-in scaler without a filter on the stage: params' upscaler for an
 * output larger on both sides (the presets': PHOTO Lanczos, ANIME and GAME
 * the edge-adaptive), Lanczos for any other size
 */
static void test_default(void)
{
    uint8_t *a, *b;
    size_t a_size, b_size;

    tf_convert(HDRG_PRESET_PHOTO, 0xffu, NULL, TF_W * 2u, TF_H * 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_PHOTO, 0xffu, "hdrg.scale.lanczos", TF_W * 2u, TF_H * 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    tf_convert(HDRG_PRESET_ANIME, 0xffu, NULL, TF_W * 2u, TF_H * 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_ANIME, 0xffu, "hdrg.scale.edge", TF_W * 2u, TF_H * 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    tf_convert(HDRG_PRESET_ANIME, 0xffu, NULL, TF_W * 2u, TF_H * 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_ANIME, 0xffu, "hdrg.scale.lanczos", TF_W * 2u, TF_H * 2u, &b, &b_size);
    TEST_ASSERT_FALSE(tf_same(a, a_size, b, b_size));

    tf_convert(HDRG_PRESET_GAME, 0xffu, NULL, TF_W * 3u, TF_H * 3u, &a, &a_size);
    tf_convert(HDRG_PRESET_GAME, 0xffu, "hdrg.scale.edge", TF_W * 3u, TF_H * 3u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    tf_convert(HDRG_PRESET_ANIME, 0xffu, NULL, TF_W / 2u, TF_H / 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_ANIME, 0xffu, "hdrg.scale.lanczos", TF_W / 2u, TF_H / 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    tf_convert(HDRG_PRESET_ANIME, 0xffu, NULL, TF_W * 2u, TF_H / 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_ANIME, 0xffu, "hdrg.scale.lanczos", TF_W * 2u, TF_H / 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    tf_convert(HDRG_PRESET_PHOTO, HDRG_UPSCALER_FSR1, NULL, TF_W * 2u, TF_H * 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_PHOTO, 0xffu, "hdrg.scale.fsr1", TF_W * 2u, TF_H * 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    tf_convert(HDRG_PRESET_ANIME, HDRG_UPSCALER_NIS, NULL, TF_W * 2u, TF_H * 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_ANIME, 0xffu, "hdrg.scale.nis", TF_W * 2u, TF_H * 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    /* NIS past 2x on a side: Lanczos */
    tf_convert(HDRG_PRESET_ANIME, HDRG_UPSCALER_NIS, NULL, TF_W * 3u, TF_H * 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_ANIME, 0xffu, "hdrg.scale.lanczos", TF_W * 3u, TF_H * 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));

    /* one side the same, the other larger: still params' upscaler (ANIME: the edge-adaptive) */
    tf_convert(HDRG_PRESET_ANIME, 0xffu, NULL, TF_W, TF_H * 2u, &a, &a_size);
    tf_convert(HDRG_PRESET_ANIME, 0xffu, "hdrg.scale.edge", TF_W, TF_H * 2u, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));
    tf_convert(HDRG_PRESET_PHOTO, HDRG_UPSCALER_FSR1, NULL, TF_W * 2u, TF_H, &a, &a_size);
    tf_convert(HDRG_PRESET_PHOTO, 0xffu, "hdrg.scale.fsr1", TF_W * 2u, TF_H, &b, &b_size);
    TEST_ASSERT_TRUE(tf_same(a, a_size, b, b_size));
}

/*
 * Lanczos (4 lobes) and bicubic reduce far past their old tap limit (512 a side: about 85x and
 * 128x): a 6000 x 4 flat picture to 50 x 4 and to 30 x 4 stays flat, its weights summing to one
 */
static void test_strong_reduction(void)
{
    static const uint32_t widths[2] = {50, 30};
    static const char *const ids[2] = {"hdrg.scale.lanczos", "hdrg.scale.bicubic"},
                             *const options[2] = {"taps=4", NULL};
    uint16_t *flat, *out;
    uint8_t k;
    size_t i, n;

    n = (size_t)6000 * 4u * 3u;
    flat = (uint16_t *)malloc(n * sizeof(uint16_t));
    out = (uint16_t *)malloc((size_t)50 * 4u * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(flat != NULL && out != NULL);

    for (i = 0; i < n; i++) {
        flat[i] = (uint16_t)(20000u + 4000u * (uint32_t)(i % 3u));
    }

    g_src_w = 6000;
    g_src_h = 4;

    for (k = 0; k < 2u; k++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, tf_scale(ids[k], options[k], flat, widths[k], 4, 4, 1, out));

        for (i = 0; i < (size_t)widths[k] * 4u * 3u; i++) {
            TEST_ASSERT_EQUAL_UINT16(20000u + 4000u * (uint32_t)(i % 3u), out[i]);
        }
    }

    g_src_w = TF_W;
    g_src_h = TF_H;
    free(out);
    free(flat);
}

/*
 * the GPU versions (those of the build with a device) against the CPU's:
 * every sample within 2 codes, at most 1 % of them off at all, at 2x and at
 * a ratio that is no whole number, with each scaler's two settings; the
 * same size is the source
 */
static void test_gpu(void)
{
    static const uint32_t sizes[3][2][2] = {
        {{192, 128}, {227, 152}}, {{192, 128}, {170, 113}}, {{192, 128}, {227, 152}}};
    static const char *const cpus[3] = {"hdrg.scale.fsr1", "hdrg.scale.nis", "hdrg.scale.edge"},
                             *const names[3] = {"fsr1", "nis", "edge"},
                             *const prefixes[3] = {"hdrg.metal.", "hdrg.vulkan.", "hdrg.webgpu."},
                             *const options[3][2] = {{"sharpen=0", "sharpen=1"},
                                                     {"sharpness=0.5", "sharpness=0"},
                                                     {"sharpen=0.25", "stretch=2:sharpen=1"}};
    hdrg_status_t status;
    uint32_t f, g, s, k, worst, off, ran;
    uint16_t *cpu, *gpu;
    size_t i, n;
    char id[48], line[160];

    cpu = (uint16_t *)malloc((size_t)227 * 152 * 3u * sizeof(uint16_t));
    gpu = (uint16_t *)malloc((size_t)227 * 152 * 3u * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(cpu);
    TEST_ASSERT_NOT_NULL(gpu);
    ran = 0;

    for (f = 0; f < 3u; f++) {
        for (g = 0; g < 3u; g++) {
            snprintf(id, sizeof(id), "%s%s", prefixes[g], names[f]);

            if (!tf_has(id)) {
                continue;
            }

            for (k = 0; k < 2u; k++) {
                for (s = 0; s < 2u; s++) {
                    n = (size_t)sizes[f][s][0] * sizes[f][s][1] * 3u;
                    status = tf_scale(id, options[f][k], g_pixels, sizes[f][s][0], sizes[f][s][1],
                                      sizes[f][s][1], 1, gpu);
                    if (status == HDRG_ERR_UNSUPPORTED || status == HDRG_ERR_PLUGIN) {
                        continue;
                    }

                    TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
                    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                          tf_scale(cpus[f], options[f][k], g_pixels, sizes[f][s][0],
                                                   sizes[f][s][1], sizes[f][s][1], 1, cpu));
                    worst = 0;
                    off = 0;

                    for (i = 0; i < n; i++) {
                        worst = HDRG_MAX(worst, (uint32_t)abs((int)gpu[i] - (int)cpu[i]));
                        off += (gpu[i] != cpu[i]) ? 1u : 0u;
                    }

                    snprintf(line, sizeof(line), "%s (%s) %ux%u: worst %u, %u of %zu off", id,
                             options[f][k], sizes[f][s][0], sizes[f][s][1], worst, off, n);
                    TEST_MESSAGE(line);
                    TEST_ASSERT_TRUE(worst <= 2u);
                    TEST_ASSERT_TRUE((size_t)off * 100u <= n);
                    ran++;
                }
            }

            /* the same size: the source */
            if (tf_scale(id, NULL, g_pixels, TF_W, TF_H, TF_H, 1, gpu) == HDRG_OK) {
                TEST_ASSERT_EQUAL_MEMORY(g_pixels, gpu,
                                         (size_t)TF_W * TF_H * 3u * sizeof(uint16_t));
            }
        }
    }

    free(cpu);
    free(gpu);

    /* a session's frames through each GPU scaler: its buffers reused alike, near the CPU's */
    for (f = 0; f < 4u; f++) {
        for (g = 0; g < 3u; g++) {
            snprintf(id, sizeof(id), "%s%s", prefixes[g], (f < 3u) ? names[f] : "lanczos");

            if (!tf_has(id)) {
                continue;
            }

            /* a device to run on (WebGPU in node has none) */
            gpu = (uint16_t *)malloc((size_t)TF_W * 2u * TF_H * 2u * 3u * sizeof(uint16_t));
            TEST_ASSERT_NOT_NULL(gpu);
            status = tf_scale(id, NULL, g_pixels, TF_W * 2u, TF_H * 2u, TF_H * 2u, 1, gpu);
            free(gpu);

            if (status == HDRG_ERR_UNSUPPORTED || status == HDRG_ERR_PLUGIN) {
                continue;
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
            gpu = tf_frames_alike(id, NULL, 2, 2);
            cpu = tf_frames_alike((f < 3u) ? cpus[f] : "hdrg.scale.lanczos", NULL, 2, 2);
            n = (size_t)TF_W * 2u * TF_H * 2u * 3u;
            worst = 0;
            off = 0;

            for (i = 0; i < n; i++) {
                worst = HDRG_MAX(worst, (uint32_t)abs((int)gpu[i] - (int)cpu[i]));
                off += (gpu[i] != cpu[i]) ? 1u : 0u;
            }

            snprintf(line, sizeof(line), "%s in a session: worst %u, %u of %zu off", id, worst, off,
                     n);
            TEST_MESSAGE(line);
            TEST_ASSERT_TRUE(worst <= 2u);
            TEST_ASSERT_TRUE((size_t)off * 100u <= n);
            free(gpu);
            free(cpu);
            ran++;
        }
    }

    if (ran == 0) {
        TEST_IGNORE_MESSAGE("no GPU");
    }
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    int result;

    g_alloc = hdrg_test_allocator();
    g_pixels = (uint16_t *)malloc((size_t)TF_W * TF_H * 3u * sizeof(uint16_t));

    if (g_pixels == NULL) {
        return 1;
    }

    tf_disc(TF_W, TF_H, g_pixels);

    UNITY_BEGIN();
    RUN_TEST(test_fsr_bands);
    RUN_TEST(test_fsr_flat);
    RUN_TEST(test_fsr_sizes);
    RUN_TEST(test_fsr_disc);
    RUN_TEST(test_nis);
    RUN_TEST(test_edge);
    RUN_TEST(test_photo);
    RUN_TEST(test_default);
    RUN_TEST(test_strong_reduction);
    RUN_TEST(test_fixed);
#if HDRG_BUILTIN_SCALE_TEMPORAL
    RUN_TEST(test_temporal);
#endif
    RUN_TEST(test_gpu);
    result = UNITY_END();

    free(g_pixels);

    return result;
}
