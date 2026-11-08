/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/math.h"
#include "hdrgrading/foundation/thread.h"
#include "session/session.h"

/*
 * The video session: with a zero window every frame's
 * output equals the still pipeline's single-layer PQ rows for the same
 * pixels, and so does a frame whose window holds only itself (the first of
 * a scene). The output does not depend on the thread count or the output
 * stride; the send / receive protocol holds with a future window (AGAIN,
 * EOF, the latency, release once per frame, pts in order); a scene cut
 * restarts the window and no window reaches across one. The frames pan
 * across a wider synthetic image one pixel a frame (windows into it, no
 * copies); the frame-to-frame change of the brightness lift is printed per
 * window. Y'CbCr frames (6.4b) render as their R'G'B' conversion does, with
 * a future window over the ingest buffers, and Y'CbCr output is the 4:2:0
 * conversion of the 16-bit rows.
 */
#define TS_W      160u
#define TS_H      96u
#define TS_FRAMES 24u
#define TS_CUT    10u         /* the cut sequence: TS_CUT bright frames, then dark ones */
#define TS_BIG_W  (2u * TS_W) /* a frame of twice the proxy side */

/* the window of the ingest and allocation tests; the tests of the filters ignore themselves */
#if HDRG_BUILTIN_STATS
#define TS_PAST     8u
#define TS_FUTURE   3u
#define TS_FILTERED true
#define TS_REQUIRE_FILTER()
#else
#define TS_PAST             0u
#define TS_FUTURE           0u
#define TS_FILTERED         false
#define TS_REQUIRE_FILTER() TEST_IGNORE_MESSAGE("built without the temporal filters")
#endif
#define TS_BIG_H (2u * TS_H)
#define TS_SEEK  4u /* a second pass started inside the first scene */

/*
 * The seek sequence at 10 frames a second (the fade-out's reach: 20 frames):
 * the bright picture panning, a cut to the dark one at 30, back to the
 * bright one at 45 that fades out from 50 to 74 (longer than the reach) and
 * stays, a strobe from 80 to 109 that stays at its dark frame until the cut
 * to the bright picture at 114, a single flash frame at 120 and a cut to the
 * dark picture at 128
 */
#define TS_SEEK_W       64u
#define TS_SEEK_H       48u
#define TS_SEEK_FRAMES  140u
#define TS_SEEK_RATE    10u
#define TS_SEEK_COUNT   6u /* the frames compared after each seek */
#define TS_SEEK_BYTES   ((size_t)TS_SEEK_W * TS_SEEK_H * 3u)
#define TS_DRIVE_FRAMES 40u   /* the seek sequence's frames the session-independence tests take */
#define TS_KEPT_LIGHTS  4096u /* the first passes' lights keep_lights holds */

/* frame sequences */
#define TS_PAN       0u /* the bright picture panning */
#define TS_SCENE     1u /* TS_CUT bright frames, then the dark picture */
#define TS_FLASH     2u /* the dark picture, a white patch of 2% of the frame on odd frames */
#define TS_NOISY     3u /* the bright picture with noise of +-3 codes on every sample, panning */
#define TS_NOISE_KEY UINT64_C(0x6e6f697379)

/* a session driven over the seek sequence's frames into rows */
typedef struct {
    const hdrg_params_t *params;
    hdrg_session_t *session;
    const uint8_t *frames;
    uint8_t *rows;
    uint32_t count, sent, received; /* sent past count: the end was sent */
    uint8_t threads;                /* the drive's own context's */
    bool lazy, failed;
} ts_drive_t;

static hdrg_session_light_t g_lights[TS_KEPT_LIGHTS]; /* keep_lights' (g_kept of them) */
static uint8_t *g_wide, *g_dark, *g_patch, *g_noisy;
static hdrg_atomic_u32_t g_mallocs; /* the hooks are called from any thread */
static uint32_t g_released, g_kept;
static float g_apl_budget; /* params_pq's APL budget, nits (0 = the preset's) */
static float g_deblock;    /* params_pq's deblocking strength ... */
static float g_deband;     /* ... and debanding (0: the session's own mechanics, no stage) */
static uint8_t g_grain;    /* params_pq's grain policy */
static uint8_t g_threads;  /* params_pq's thread count, the contexts' */

/* a first pass's output lights kept in g_lights: their copy (NULL: none) */
static const hdrg_session_light_t *keep_lights(const hdrg_session_output_t *out)
{
    hdrg_session_light_t *kept = g_lights + g_kept;

    if (out->light_count == 0) {
        return NULL;
    }

    TEST_ASSERT_TRUE(out->lights != NULL && g_kept + out->light_count <= TS_KEPT_LIGHTS);
    memcpy(kept, out->lights, out->light_count * sizeof(*kept));
    g_kept += out->light_count;

    return kept;
}

static void count_release(void *opaque)
{
    (void)opaque;
    g_released++;
}

static void *count_malloc(size_t size, void *user)
{
    (void)user;
    (void)hdrg_atomic_u32_fetch_add_relaxed(&g_mallocs, 1u);

    return malloc(size);
}

static void *count_realloc(void *ptr, size_t size, void *user)
{
    (void)user;
    (void)hdrg_atomic_u32_fetch_add_relaxed(&g_mallocs, 1u);

    return realloc(ptr, size);
}

static void count_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}

/* a session config of RGB in and PQ R'G'B' of bits out */
static hdrg_session_config_t rgb_config(uint16_t past, uint16_t future, uint8_t bits)
{
    hdrg_session_config_t config;

    hdrg_session_config_init(&config);
    config.past = past;
    config.future = future;
    config.output.bits = bits;
    config.proxy = 0;

    return config;
}

static void params_pq(hdrg_params_t *params, uint8_t threads)
{
    hdrg_params_init(params, HDRG_PRESET_PHOTO);
    g_threads = threads;

    if (g_apl_budget > 0.0f) {
        params->apl_budget_nits = g_apl_budget;
    }

    params->grain_policy = g_grain;
    params->deblock_strength = g_deblock;
    params->deband_strength = g_deband;
    params->mctf_strength = 0.0f;
}

/* a session's output form for the still pipeline: single-layer PQ */
static void encode_pq(hdrg_encode_params_t *encode, bool dither)
{
    hdrg_encode_params_init(encode);
    encode->output_mode = HDRG_OUTPUT_PQ;
    encode->dither = dither;
}

/* the frame format: a TS_W x TS_H window into a wide image */
static void format_of(hdrg_raw_image_t *format)
{
    memset(format, 0, sizeof(*format));
    format->width = TS_W;
    format->height = TS_H;
    format->bit_depth = 8;
    format->stride = (size_t)(TS_W + TS_FRAMES) * 3u;
    format->primaries = HDRG_PRIMARIES_BT709;
    format->transfer = HDRG_TRANSFER_SRGB;
}

/* frame t of a sequence */
static const uint8_t *frame_pixels(uint32_t t, uint8_t sequence)
{
    if (sequence == TS_FLASH) {
        return ((t % 2u == 1u) ? g_patch : g_dark) + (size_t)t * 3u;
    }

    if (sequence == TS_NOISY) {
        return g_noisy + (size_t)t * 3u;
    }

    return ((sequence == TS_SCENE && t >= TS_CUT) ? g_dark : g_wide) + (size_t)t * 3u;
}

/* the still pipeline's PQ rows of pixels at out_w x out_h (0 x 0: the input's size) */
static void still_rows_at(const uint8_t *pixels, uint32_t out_w, uint32_t out_h, uint8_t *rows)
{
    hdrg_params_t params, effective;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t caps;
    hdrg_raw_image_t format;
    hdrg_color_t color;
    hdrg_pipeline_t pipeline;
    hdrg_context_t *ctx;
    uint32_t y0;

    params_pq(&params, 1);
    encode_pq(&encode, true);
    encode.output_width = out_w;
    encode.output_height = out_h;
    TEST_ASSERT_TRUE(hdrg_params_resolve(&params, &effective));
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_display_caps_init(&caps);
    format_of(&format);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_open(ctx, &format, &color, &effective, &encode,
                                                      &caps, 0, HDRG_LATENCY_OFFLINE, &pipeline));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_frame(ctx, &pipeline, pixels));

    for (y0 = 0; y0 < pipeline.height; y0 += HDRG_APPLY_BAND_ROWS) {
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, hdrg_pipeline_rows(ctx, &pipeline, y0,
                                        HDRG_MIN(HDRG_APPLY_BAND_ROWS, pipeline.height - y0),
                                        rows + (size_t)y0 * pipeline.width * 3u));
    }

    hdrg_pipeline_close(ctx, &pipeline);
    hdrg_color_free(&ctx->alloc, &color);
    hdrg_context_destroy(ctx);
}

/* the still pipeline's PQ rows of pixels */
static void still_rows(const uint8_t *pixels, uint8_t *rows)
{
    still_rows_at(pixels, 0, 0, rows);
}

/*
 * Every frame through a session: receive while frames come out, send the
 * next one (or the end) on AGAIN. The rows of all frames at rows (TS_FRAMES
 * x TS_H rows of stride), their information at info.
 */
static void session_rows(const hdrg_session_config_t *window, uint8_t threads, uint8_t sequence,
                         size_t stride, uint8_t *rows, hdrg_session_output_t *info)
{
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t sent, received;

    params_pq(&params, threads);
    format_of(&format);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, window, &session));
    TEST_ASSERT_EQUAL_UINT32(window->future, hdrg_session_latency(session));

    sent = 0;
    received = 0;
    for (;;) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows + (size_t)received * TS_H * stride;
        out.strides[0] = stride;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.planes[0] = frame_pixels(sent, sequence);
            in.pts = (int64_t)sent * 1001;

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TS_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(out.pts == (int64_t)received * 1001);
        TEST_ASSERT_TRUE(out.apl_nits > 0.0f && out.sdr_apl_nits > 0.0f);
        TEST_ASSERT_TRUE(out.noise_sigma >= 0.0f && out.blockiness > 0.0f);

        /* a frame comes out once its future window is in */
        TEST_ASSERT_TRUE(sent >= HDRG_MIN(received + 1u + window->future, TS_FRAMES));

        if (info != NULL) {
            info[received] = out;
        }

        received++;
    }

    TEST_ASSERT_EQUAL_UINT32(TS_FRAMES, received);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

static void test_zero_window_matches_still(void)
{
    const size_t stride = (size_t)TS_W * 3u;
    hdrg_session_config_t window;
    uint32_t t;
    uint8_t *rows, *want;

    /* a frame within the proxy side is analyzed at full resolution */
    window = rgb_config(0, 0, 8);
    window.proxy = TS_W;

    rows = (uint8_t *)malloc(stride * TS_H * TS_FRAMES);
    want = (uint8_t *)malloc(stride * TS_H);
    TEST_ASSERT_NOT_NULL(rows);
    TEST_ASSERT_NOT_NULL(want);

    session_rows(&window, 1, TS_PAN, stride, rows, NULL);

    for (t = 0; t < TS_FRAMES; t += 5) {
        still_rows(frame_pixels(t, TS_PAN), want);
        TEST_ASSERT_EQUAL_MEMORY(want, rows + (size_t)t * TS_H * stride, stride * TS_H);
    }

    free(rows);
    free(want);
}

/*
 * The frames of the pan through a session of config (its output_width x
 * output_height: out_w x out_h) into rows, frame t of R'G'B' at t rows
 * of frames; 4:2:0 (a frame): its planes at offsets of rows, of strides
 */
static void scaled_frames(const hdrg_session_config_t *config, const hdrg_display_caps_t *caps,
                          uint32_t frames, uint32_t out_w, uint32_t out_h, uint8_t *rows,
                          const size_t *offsets, const size_t *strides)
{
    const size_t row = (size_t)out_w * 3u * (config->output.bits / 8u);
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t t;
    uint8_t p;

    params_pq(&params, 1);
    format_of(&format);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       caps, config, &session));

    for (t = 0; t < frames; t++) {
        memset(&in, 0, sizeof(in));
        in.planes[0] = frame_pixels(t, TS_PAN);
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows + (size_t)t * row * out_h;
        out.strides[0] = row;

        for (p = 0; offsets != NULL && p < 3u; p++) {
            out.planes[p] = rows + offsets[p];
            out.strides[p] = strides[p];
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &out));
        TEST_ASSERT_TRUE(out.pts == 0 && out.max_nits > 0.0f);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_EOF, hdrg_session_receive_frame(session, &out));

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/*
 * The SCALE stage in a session: each frame of a zero window
 * at another output size (one side given: the aspect ratio kept; both:
 * smaller) is the still pipeline's at that size; the 4:2:0 output at the
 * scaled size is its 16-bit R'G'B' converted
 */
static void test_scaled_matches_still(void)
{
    const uint32_t up_w = TS_W * 3u / 2u, up_h = TS_H * 3u / 2u, down_w = TS_W / 2u,
                   down_h = TS_H / 2u;
    const size_t up = (size_t)up_w * up_h * 3u;
    hdrg_session_config_t config;
    hdrg_display_caps_t caps;
    hdrg_yuv_t yuv;
    uint32_t t;
    uint16_t *rgb;
    uint8_t *rows, *want, *planes, *expected, p;
    size_t chroma, offsets[3];

    rows = (uint8_t *)malloc(up * TS_FRAMES);
    want = (uint8_t *)malloc(up);
    rgb = (uint16_t *)malloc(up * sizeof(uint16_t));
    chroma = (size_t)(up_w / 2u) * (up_h / 2u) * 2u;
    planes = (uint8_t *)malloc((size_t)up_w * up_h * 2u + 2u * chroma);
    expected = (uint8_t *)malloc((size_t)up_w * up_h * 2u + 2u * chroma);
    TEST_ASSERT_TRUE(rows != NULL && want != NULL && rgb != NULL && planes != NULL &&
                     expected != NULL);

    config = rgb_config(0, 0, 8);
    config.proxy = TS_W;
    config.output_width = up_w;
    scaled_frames(&config, NULL, TS_FRAMES, up_w, up_h, rows, NULL, NULL);

    for (t = 0; t < TS_FRAMES; t += 5) {
        still_rows_at(frame_pixels(t, TS_PAN), up_w, 0, want);
        TEST_ASSERT_EQUAL_MEMORY(want, rows + (size_t)t * up, up);
    }

    config.output_width = down_w;
    config.output_height = down_h;
    scaled_frames(&config, NULL, 1, down_w, down_h, rows, NULL, NULL);
    still_rows_at(frame_pixels(0, TS_PAN), down_w, down_h, want);
    TEST_ASSERT_EQUAL_MEMORY(want, rows, (size_t)down_w * down_h * 3u);

    /* 4:2:0 at the scaled size */
    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;
    config = rgb_config(0, 0, 16);
    config.proxy = TS_W;
    config.dither_off = true;
    config.output_width = up_w;
    config.output_height = up_h;
    scaled_frames(&config, &caps, 1, up_w, up_h, (uint8_t *)rgb, NULL, NULL);

    memset(&yuv, 0, sizeof(yuv));
    yuv.width = up_w;
    yuv.height = up_h;
    yuv.layout = HDRG_YUV_420;
    yuv.matrix = HDRG_MATRIX_BT2020;
    yuv.bits = 10;
    yuv.strides[0] = (size_t)up_w * 2u;
    yuv.strides[1] = (size_t)(up_w / 2u) * 2u;
    yuv.strides[2] = yuv.strides[1];
    offsets[0] = 0;
    offsets[1] = (size_t)up_w * up_h * 2u;
    offsets[2] = offsets[1] + chroma;

    for (p = 0; p < 3u; p++) {
        yuv.planes[p] = expected + offsets[p];
    }

    hdrg_rgb16_to_yuv420(NULL, rgb, 0, up_h, &yuv);

    config.output.layout = HDRG_YUV_420;
    config.output.matrix = HDRG_MATRIX_BT2020;
    config.output.bits = 10;
    config.output.chroma_loc = HDRG_CHROMA_LEFT;
    scaled_frames(&config, &caps, 1, up_w, up_h, planes, offsets, yuv.strides);
    TEST_ASSERT_EQUAL_MEMORY(expected, planes, (size_t)up_w * up_h * 2u + 2u * chroma);

    free(rows);
    free(want);
    free(rgb);
    free(planes);
    free(expected);
}

/* threads, a padded output stride (the band copy) and a future window give the same rows */
static void test_threads_and_stride_identical(void)
{
    const hdrg_session_config_t window = rgb_config(8, 4, 8);
    const size_t stride = (size_t)TS_W * 3u, padded = stride + 13u;
    uint32_t i;
    uint8_t *one, *four, *wide;

    TS_REQUIRE_FILTER();

    HDRG_TEST_REQUIRE_THREADS();

    one = (uint8_t *)malloc(stride * TS_H * TS_FRAMES);
    four = (uint8_t *)malloc(stride * TS_H * TS_FRAMES);
    wide = (uint8_t *)malloc(padded * TS_H * TS_FRAMES);
    TEST_ASSERT_NOT_NULL(one);
    TEST_ASSERT_NOT_NULL(four);
    TEST_ASSERT_NOT_NULL(wide);

    session_rows(&window, 1, TS_PAN, stride, one, NULL);
    session_rows(&window, 4, TS_PAN, stride, four, NULL);
    session_rows(&window, 4, TS_PAN, padded, wide, NULL);
    TEST_ASSERT_EQUAL_MEMORY(one, four, stride * TS_H * TS_FRAMES);

    for (i = 0; i < TS_H * TS_FRAMES; i++) {
        TEST_ASSERT_EQUAL_MEMORY(one + (size_t)i * stride, wide + (size_t)i * padded, stride);
    }

    free(one);
    free(four);
    free(wide);
}

/*
 * A cut restarts the window: the cut frames are found, the first frame of
 * each scene equals the still output, and the last frame before the cut
 * averages over its own scene only (the same with or without a future)
 */
static void test_scene_cut(void)
{
    const hdrg_session_config_t past = rgb_config(8, 0, 8), ahead = rgb_config(8, 4, 8);
    const size_t stride = (size_t)TS_W * 3u;
    hdrg_session_output_t info[TS_FRAMES], info_ahead[TS_FRAMES];
    uint32_t t;
    uint8_t *rows, *want;

    TS_REQUIRE_FILTER();

    rows = (uint8_t *)malloc(stride * TS_H * TS_FRAMES);
    want = (uint8_t *)malloc(stride * TS_H);
    TEST_ASSERT_NOT_NULL(rows);
    TEST_ASSERT_NOT_NULL(want);

    session_rows(&past, 1, TS_SCENE, stride, rows, info);

    for (t = 0; t < TS_FRAMES; t++) {
        TEST_ASSERT_EQUAL(t == 0 || t == TS_CUT, info[t].scene_cut);
    }

    still_rows(frame_pixels(0, TS_SCENE), want);
    TEST_ASSERT_EQUAL_MEMORY(want, rows, stride * TS_H);
    still_rows(frame_pixels(TS_CUT, TS_SCENE), want);
    TEST_ASSERT_EQUAL_MEMORY(want, rows + (size_t)TS_CUT * TS_H * stride, stride * TS_H);

    session_rows(&ahead, 1, TS_SCENE, stride, rows, info_ahead);
    TEST_ASSERT_TRUE(info_ahead[TS_CUT - 1u].anchor_used == info[TS_CUT - 1u].anchor_used);
    TEST_ASSERT_TRUE(info_ahead[TS_CUT].scene_cut);

    free(rows);
    free(want);
}

static void test_protocol(void)
{
    const hdrg_session_config_t window = rgb_config(2, 3, 8);
    hdrg_params_t params;
    hdrg_display_caps_t caps;
    hdrg_raw_image_t format;
    hdrg_session_config_t bad;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t i;
    uint8_t *rows;

    TS_REQUIRE_FILTER();

    params_pq(&params, 1);
    format_of(&format);
    rows = (uint8_t *)malloc((size_t)TS_W * 3u * TS_H);
    TEST_ASSERT_NOT_NULL(rows);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &window, &session));
    TEST_ASSERT_EQUAL_UINT32(3, hdrg_session_latency(session));

    memset(&in, 0, sizeof(in));
    in.planes[0] = frame_pixels(0, TS_PAN);
    in.release = count_release;
    memset(&out, 0, sizeof(out));
    out.planes[0] = rows;
    out.strides[0] = (size_t)TS_W * 3u;
    g_released = 0;

    /* nothing comes out until the future window is in; then the queue is full */
    for (i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_AGAIN, hdrg_session_receive_frame(session, &out));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_AGAIN, hdrg_session_send_frame(session, &in));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &out));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_AGAIN, hdrg_session_receive_frame(session, &out));
    TEST_ASSERT_EQUAL_UINT32(1, g_released);

    /* the end drains the rest, then EOF; no frame after the end */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE, hdrg_session_send_frame(session, &in));

    for (i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &out));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_EOF, hdrg_session_receive_frame(session, &out));
    TEST_ASSERT_EQUAL_UINT32(4, g_released);

    /* a short output stride is refused */
    out.strides[0] = (size_t)TS_W * 3u - 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_receive_frame(session, &out));

    hdrg_session_destroy(session);

    /* destroy releases what is still queued */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &window, &session));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
    hdrg_session_destroy(session);
    TEST_ASSERT_EQUAL_UINT32(6, g_released);

    /* broken formats are refused; the default window */
    format.stride = (size_t)TS_W * 3u - 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, NULL, &session));
    format_of(&format);
    format.bit_depth = 10;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, NULL, &session));
    format_of(&format);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, NULL, &session));
    TEST_ASSERT_EQUAL_UINT32(HDRG_SESSION_PAST, hdrg_session_latency(session));
    hdrg_session_destroy(session);

    /* a config or display of another version; NULL params are the defaults */
    bad = window;
    bad.struct_size = 4;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    hdrg_display_caps_init(&caps);
    caps.struct_size = 4;
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, &caps, NULL, &session));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), NULL, NULL, NULL, &session));
    hdrg_session_destroy(session);

    free(rows);
    hdrg_context_destroy(ctx);
}

/*
 * The largest frame-to-frame change, in stops, of the anchor the plan used
 * and of the lift, over a sequence
 */
static void changes(const hdrg_session_config_t *window, uint8_t sequence, float *anchor,
                    float *lift)
{
    const size_t stride = (size_t)TS_W * 3u;
    hdrg_session_output_t info[TS_FRAMES];
    uint32_t t;
    uint8_t *rows;

    rows = (uint8_t *)malloc(stride * TS_H * TS_FRAMES);
    TEST_ASSERT_NOT_NULL(rows);

    session_rows(window, 1, sequence, stride, rows, info);

    *anchor = 0.0f;
    *lift = 0.0f;
    for (t = 1; t < TS_FRAMES; t++) {
        *anchor = HDRG_TEST_MAX(
            *anchor, fabsf(log2f(info[t].anchor_used) - log2f(info[t - 1u].anchor_used)));
        *lift =
            HDRG_TEST_MAX(*lift, fabsf(log2f(info[t].apl_nits / info[t].sdr_apl_nits) -
                                       log2f(info[t - 1u].apl_nits / info[t - 1u].sdr_apl_nits)));
    }

    free(rows);
}

/*
 * A highlight blinking in a dark scene moves its p99.5 by four stops every
 * frame: the window steadies the anchor the plan uses. The flicker criterion:
 * a pan's brightness lift changes by less than 0.01 stop a frame.
 */
static void test_flicker(void)
{
    const hdrg_session_config_t none = rgb_config(0, 0, 8), window = rgb_config(8, 4, 8);
    char line[160];
    float anchor_none, lift_none, anchor_window, lift_window, anchor_pan, lift_pan;

    TS_REQUIRE_FILTER();

    changes(&none, TS_FLASH, &anchor_none, &lift_none);
    changes(&window, TS_FLASH, &anchor_window, &lift_window);
    changes(&window, TS_PAN, &anchor_pan, &lift_pan);

    (void)snprintf(line, sizeof(line),
                   "blinking highlight: anchor change a frame (max) %.3f -> %.3f stops, lift "
                   "%.4f -> %.4f (window 8/4); pan lift %.5f",
                   (double)anchor_none, (double)anchor_window, (double)lift_none,
                   (double)lift_window, (double)lift_pan);
    TEST_MESSAGE(line);
    TEST_ASSERT_TRUE(anchor_window < 0.25f * anchor_none);
    TEST_ASSERT_TRUE(lift_pan < 0.01f);
}

/* one frame through a session of config (no window) on caps (NULL: the file profile) */
static void render_one(const hdrg_session_config_t *config, const hdrg_display_caps_t *caps,
                       bool dither, const uint8_t *pixels, uint8_t *rows,
                       hdrg_session_output_t *info)
{
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t local;
    hdrg_session_frame_t in;
    hdrg_session_t *session;
    hdrg_context_t *ctx;

    params_pq(&params, 1);
    format_of(&format);
    local = *config;
    local.dither_off = !dither;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       caps, &local, &session));

    memset(&in, 0, sizeof(in));
    in.planes[0] = pixels;
    memset(info, 0, sizeof(*info));
    info->planes[0] = rows;
    info->strides[0] = (size_t)TS_W * 3u * (config->output.bits / 8u);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, info));

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/*
 * 16-bit rows are the 8-bit ones at 257 times the precision (no dither: within
 * a code), and the measured light is consistent
 */
static void test_depth16_and_light(void)
{
    const hdrg_session_config_t eight = rgb_config(0, 0, 8), sixteen = rgb_config(0, 0, 16);
    const size_t samples = (size_t)TS_W * 3u * TS_H;
    hdrg_session_output_t info8, info16;
    uint16_t *wide;
    uint8_t *narrow;
    size_t i;

    narrow = (uint8_t *)malloc(samples);
    wide = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(narrow);
    TEST_ASSERT_NOT_NULL(wide);

    render_one(&eight, NULL, false, frame_pixels(3, TS_PAN), narrow, &info8);
    render_one(&sixteen, NULL, false, frame_pixels(3, TS_PAN), (uint8_t *)wide, &info16);

    for (i = 0; i < samples; i++) {
        TEST_ASSERT_TRUE(fabs((double)wide[i] / 257.0 - (double)narrow[i]) <= 1.0);
    }

    TEST_ASSERT_TRUE(info16.max_nits == info8.max_nits && info16.avg_nits == info8.avg_nits);
    TEST_ASSERT_TRUE(info16.avg_nits > 0.0f && info16.max_nits >= info16.avg_nits);
    TEST_ASSERT_TRUE(info16.max_nits <= info16.peak_nits * 1.001f);

    free(narrow);
    free(wide);
}

/*
 * A BT.2020 container: neutrals keep their codes (the whites coincide),
 * colors move to the wider primaries
 */
static void test_bt2020_container(void)
{
    const hdrg_session_config_t sixteen = rgb_config(0, 0, 16);
    const size_t samples = (size_t)TS_W * 3u * TS_H;
    hdrg_params_t params;
    hdrg_display_caps_t p3, bt2020;
    hdrg_session_output_t info;
    uint16_t *a, *b;
    uint8_t *gray;
    size_t i;

    a = (uint16_t *)malloc(samples * sizeof(uint16_t));
    b = (uint16_t *)malloc(samples * sizeof(uint16_t));
    gray = (uint8_t *)malloc((size_t)(TS_W + TS_FRAMES) * TS_H * 3u);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(gray);

    params_pq(&params, 1);
    hdrg_display_caps_init(&p3);
    bt2020 = p3;
    bt2020.gamut = HDRG_GAMUT_BT2020;

    for (i = 0; i < (size_t)(TS_W + TS_FRAMES) * TS_H * 3u; i++) {
        gray[i] = g_wide[i - i % 3u + 2u];
    }

    render_one(&sixteen, &p3, false, gray, (uint8_t *)a, &info);
    render_one(&sixteen, &bt2020, false, gray, (uint8_t *)b, &info);
    TEST_ASSERT_EQUAL_MEMORY(a, b, samples * sizeof(uint16_t));

    render_one(&sixteen, &p3, false, frame_pixels(0, TS_PAN), (uint8_t *)a, &info);
    render_one(&sixteen, &bt2020, false, frame_pixels(0, TS_PAN), (uint8_t *)b, &info);
    TEST_ASSERT_TRUE(memcmp(a, b, samples * sizeof(uint16_t)) != 0);

    free(a);
    free(b);
    free(gray);
}

/*
 * frames through a session into rows (frame_bytes apart; planes at offsets
 * of strides), their information at info (NULL = none)
 */
static void drive(hdrg_session_t *session, const hdrg_session_frame_t *frames, uint8_t planes,
                  const size_t offsets[3], const size_t strides[3], size_t frame_bytes,
                  uint8_t *rows, hdrg_session_output_t *info)
{
    hdrg_session_output_t out;
    hdrg_status_t status;
    uint32_t sent, received;
    uint8_t p;

    sent = 0;
    received = 0;
    for (;;) {
        memset(&out, 0, sizeof(out));
        for (p = 0; p < planes; p++) {
            out.planes[p] = rows + (size_t)received * frame_bytes + offsets[p];
            out.strides[p] = strides[p];
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(
                                               session, (sent < TS_FRAMES) ? &frames[sent] : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(out.pts == (int64_t)received);

        if (info != NULL) {
            info[received] = out;
            info[received].lights = keep_lights(&out);
        }

        received++;
    }

    TEST_ASSERT_EQUAL_UINT32(TS_FRAMES, received);
}

/* frame t of a sequence as 16-bit R'G'B' (codes x 257) */
static void sequence_rgb16(uint32_t t, uint8_t sequence, uint16_t *rgb)
{
    const uint8_t *pixels = frame_pixels(t, sequence);
    uint32_t x, y;
    uint8_t c;

    for (y = 0; y < TS_H; y++) {
        for (x = 0; x < TS_W; x++) {
            for (c = 0; c < 3u; c++) {
                rgb[((size_t)y * TS_W + x) * 3u + c] =
                    (uint16_t)(pixels[(size_t)y * (TS_W + TS_FRAMES) * 3u + (size_t)x * 3u + c] *
                               257u);
            }
        }
    }
}

/* an orange field and a blue one with a dark 2-pixel outline between them, over x < 48 of rgb */
static void outline_into(uint16_t *rgb, uint32_t t)
{
    uint32_t x, y, edge;
    uint16_t *px;

    for (y = 0; y < TS_H; y++) {
        edge = 20u + (y + t) / 3u;

        for (x = 0; x < 48u; x++) {
            px = rgb + ((size_t)y * TS_W + x) * 3u;
            px[0] = (x < edge) ? 60000u : ((x < edge + 2u) ? 5000u : 12000u);
            px[1] = (x < edge) ? 36000u : ((x < edge + 2u) ? 4000u : 30000u);
            px[2] = (x < edge) ? 12000u : ((x < edge + 2u) ? 4500u : 52000u);
        }
    }
}

/* a 4:2:0 picture of TS_W x TS_H at planes (Y, then Cb, then Cr); returns its bytes */
static size_t yuv420_at(uint8_t *planes, uint8_t matrix, uint8_t bits, hdrg_yuv_t *yuv,
                        size_t offsets[3])
{
    const size_t bytes = (bits > 8u) ? 2u : 1u;
    uint8_t p;

    memset(yuv, 0, sizeof(*yuv));
    yuv->width = TS_W;
    yuv->height = TS_H;
    yuv->layout = HDRG_YUV_420;
    yuv->matrix = matrix;
    yuv->bits = bits;
    yuv->strides[0] = (size_t)TS_W * bytes;
    yuv->strides[1] = (size_t)(TS_W / 2u) * bytes;
    yuv->strides[2] = yuv->strides[1];
    offsets[0] = 0;
    offsets[1] = yuv->strides[0] * TS_H;
    offsets[2] = offsets[1] + yuv->strides[1] * (TS_H / 2u);

    for (p = 0; p < 3u; p++) {
        yuv->planes[p] = (planes != NULL) ? planes + offsets[p] : NULL;
    }

    return offsets[2] + yuv->strides[2] * (TS_H / 2u);
}

/*
 * The pan as 4:2:0 10-bit video-range BT.709 frames renders as their R'G'B'
 * (the conversion the ingest runs) does, with a future window over the
 * ingest buffers; the caller's planes are released when they are sent. At
 * the top-left chroma location, as the conversion at that location; with
 * the segmented chroma fit (params), as the conversion with that fit
 */
static void test_yuv_input(void)
{
    const hdrg_session_pixfmt_t yuv709 = {
        HDRG_YUV_420, HDRG_MATRIX_BT709, 10, HDRG_CHROMA_LEFT, false, false, false};
    const hdrg_session_config_t rgb_window = rgb_config(TS_PAST, TS_FUTURE, 8);
    const size_t row = (size_t)TS_W * 3u, rgb_offsets[3] = {0, 0, 0}, rgb_strides[3] = {row, 0, 0};
    hdrg_params_t params;
    hdrg_session_format_t format;
    hdrg_session_config_t yuv_config;
    hdrg_session_frame_t yuv_frames[TS_FRAMES], rgb_frames[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_yuv_t yuv;
    uint32_t t;
    uint16_t *rgb, *wide;
    uint8_t *planes, *want, *got, p;
    size_t offsets[3], bytes;
    float *scratch;

    bytes = yuv420_at(NULL, HDRG_MATRIX_BT709, 10, &yuv, offsets);
    planes = (uint8_t *)malloc(bytes * TS_FRAMES);
    rgb = (uint16_t *)malloc(row * TS_H * TS_FRAMES * sizeof(uint16_t));
    wide = (uint16_t *)malloc(row * TS_H * sizeof(uint16_t));
    want = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    got = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    yuv.fit = HDRG_CHROMA_FIT_SEGMENTED;
    scratch = (float *)malloc(hdrg_yuv_scratch(&yuv) * sizeof(float));
    TEST_ASSERT_TRUE(planes != NULL && rgb != NULL && wide != NULL && want != NULL && got != NULL &&
                     scratch != NULL);

    memset(yuv_frames, 0, sizeof(yuv_frames));
    memset(rgb_frames, 0, sizeof(rgb_frames));

    for (t = 0; t < TS_FRAMES; t++) {
        sequence_rgb16(t, TS_PAN, wide);
        (void)yuv420_at(planes + (size_t)t * bytes, HDRG_MATRIX_BT709, 10, &yuv, offsets);
        hdrg_rgb16_to_yuv420(NULL, wide, 0, TS_H, &yuv);
        hdrg_yuv_to_rgb16(NULL, &yuv, scratch, rgb + (size_t)t * row * TS_H,
                          (size_t)yuv.width * 6u);

        for (p = 0; p < 3u; p++) {
            yuv_frames[t].planes[p] = (const uint8_t *)yuv.planes[p];
            yuv_frames[t].strides[p] = yuv.strides[p];
        }

        yuv_frames[t].pts = (int64_t)t;
        yuv_frames[t].release = count_release;
        rgb_frames[t].planes[0] = (const uint8_t *)(const void *)(rgb + (size_t)t * row * TS_H);
        rgb_frames[t].pts = (int64_t)t;
    }

    params_pq(&params, 2);
    yuv_config = rgb_window;
    yuv_config.input = yuv709;
    memset(&format, 0, sizeof(format));
    format.width = TS_W;
    format.height = TS_H;
    format.bit_depth = 16;
    format.stride = row * 2u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &rgb_window, &session));
    drive(session, rgb_frames, 1, rgb_offsets, rgb_strides, row * TS_H, want, NULL);
    hdrg_session_destroy(session);

    /* the Y'CbCr input reads none of the format's bit depth and stride */
    format.bit_depth = 0;
    format.stride = 0;
    g_released = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &yuv_config, &session));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &yuv_frames[0]));
    TEST_ASSERT_EQUAL_UINT32(1, g_released);
    hdrg_session_destroy(session);
    TEST_ASSERT_EQUAL_UINT32(1, g_released);

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &yuv_config, &session));
    drive(session, yuv_frames, 1, rgb_offsets, rgb_strides, row * TS_H, got, NULL);
    TEST_ASSERT_EQUAL_UINT32(1u + TS_FRAMES, g_released);
    TEST_ASSERT_EQUAL_MEMORY(want, got, row * TS_H * TS_FRAMES);
    hdrg_session_destroy(session);

    /* planes that do not fit are refused when sent, and not released */
    yuv_frames[0].strides[1] = TS_W / 2u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &yuv_config, &session));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &yuv_frames[0]));
    hdrg_session_destroy(session);
    TEST_ASSERT_EQUAL_UINT32(1u + TS_FRAMES, g_released);

    /* the chroma location the input names (got holds the MPEG-2 location's frames) */
    yuv_frames[0].strides[1] = yuv.strides[1];

    for (t = 0; t < TS_FRAMES; t++) {
        (void)yuv420_at(planes + (size_t)t * bytes, HDRG_MATRIX_BT709, 10, &yuv, offsets);
        yuv.siting = HDRG_CHROMA_TOPLEFT;
        hdrg_yuv_to_rgb16(NULL, &yuv, scratch, rgb + (size_t)t * row * TS_H,
                          (size_t)yuv.width * 6u);
    }

    format.bit_depth = 16;
    format.stride = row * 2u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &rgb_window, &session));
    drive(session, rgb_frames, 1, rgb_offsets, rgb_strides, row * TS_H, want, NULL);
    hdrg_session_destroy(session);

    TEST_ASSERT_TRUE(memcmp(want, got, row * TS_H * TS_FRAMES) != 0);

    format.bit_depth = 0;
    format.stride = 0;
    yuv_config.input.chroma_loc = HDRG_CHROMA_TOPLEFT;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &yuv_config, &session));
    drive(session, yuv_frames, 1, rgb_offsets, rgb_strides, row * TS_H, got, NULL);
    TEST_ASSERT_EQUAL_MEMORY(want, got, row * TS_H * TS_FRAMES);
    hdrg_session_destroy(session);

    /*
     * the segmented chroma fit the params name, over frames with an outline
     * between two fields (where it bends: wide holds the line's conversion)
     */
    for (t = 0; t < TS_FRAMES; t++) {
        sequence_rgb16(t, TS_PAN, wide);
        outline_into(wide, t);
        (void)yuv420_at(planes + (size_t)t * bytes, HDRG_MATRIX_BT709, 10, &yuv, offsets);
        hdrg_rgb16_to_yuv420(NULL, wide, 0, TS_H, &yuv);
        yuv.siting = HDRG_CHROMA_TOPLEFT;
        hdrg_yuv_to_rgb16(NULL, &yuv, scratch, wide, (size_t)yuv.width * 6u);
        yuv.fit = HDRG_CHROMA_FIT_SEGMENTED;
        hdrg_yuv_to_rgb16(NULL, &yuv, scratch, rgb + (size_t)t * row * TS_H,
                          (size_t)yuv.width * 6u);

        TEST_ASSERT_TRUE(memcmp(wide, rgb + (size_t)t * row * TS_H, row * TS_H * 2u) != 0);
    }

    params.chroma_fit = HDRG_CHROMA_FIT_SEGMENTED;
    format.bit_depth = 16;
    format.stride = row * 2u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &rgb_window, &session));
    drive(session, rgb_frames, 1, rgb_offsets, rgb_strides, row * TS_H, want, NULL);
    hdrg_session_destroy(session);

    format.bit_depth = 0;
    format.stride = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &yuv_config, &session));
    drive(session, yuv_frames, 1, rgb_offsets, rgb_strides, row * TS_H, got, NULL);
    TEST_ASSERT_EQUAL_MEMORY(want, got, row * TS_H * TS_FRAMES);
    hdrg_session_destroy(session);

    hdrg_context_destroy(ctx);
    free(planes);
    free(rgb);
    free(wide);
    free(want);
    free(got);
    free(scratch);
}

/*
 * 4:2:0 10-bit output is the conversion of the 16-bit rows (BT.2020 matrix,
 * video range); it needs the BT.2020 container and matrix
 */
static void test_yuv_output(void)
{
    const hdrg_session_pixfmt_t yuv2020 = {
        HDRG_YUV_420, HDRG_MATRIX_BT2020, 10, HDRG_CHROMA_LEFT, false, false, false};
    const hdrg_session_config_t sixteen = rgb_config(0, 0, 16);
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_display_caps_t caps;
    hdrg_session_config_t yuv_config, bad;
    hdrg_session_frame_t in;
    hdrg_session_output_t info;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_yuv_t want;
    uint16_t *rgb;
    uint8_t *expected, *got, p;
    size_t offsets[3], bytes;

    params_pq(&params, 1);
    yuv_config = sixteen;
    yuv_config.output = yuv2020;
    yuv_config.dither_off = true;
    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;
    format_of(&format);

    bytes = yuv420_at(NULL, HDRG_MATRIX_BT2020, 10, &want, offsets);
    rgb = (uint16_t *)malloc((size_t)TS_W * 3u * TS_H * sizeof(uint16_t));
    expected = (uint8_t *)malloc(bytes);
    got = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(rgb != NULL && expected != NULL && got != NULL);

    render_one(&sixteen, &caps, false, frame_pixels(0, TS_PAN), (uint8_t *)rgb, &info);
    (void)yuv420_at(expected, HDRG_MATRIX_BT2020, 10, &want, offsets);
    hdrg_rgb16_to_yuv420(NULL, rgb, 0, TS_H, &want);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       &caps, &yuv_config, &session));

    memset(&in, 0, sizeof(in));
    in.planes[0] = frame_pixels(0, TS_PAN);
    memset(&info, 0, sizeof(info));

    for (p = 0; p < 3u; p++) {
        info.planes[p] = got + offsets[p];
        info.strides[p] = want.strides[p];
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));

    /* a chroma stride short of a row is refused */
    info.strides[2] = want.strides[2] - 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_receive_frame(session, &info));
    info.strides[2] = want.strides[2];
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &info));
    TEST_ASSERT_EQUAL_MEMORY(expected, got, bytes);
    TEST_ASSERT_TRUE(info.max_nits > 0.0f);
    hdrg_session_destroy(session);

    /*
     * the P3 container, another matrix, layout or chroma location, 10-bit
     * R'G'B' and an unknown input chroma location are refused
     */
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, NULL, &yuv_config, &session));
    bad = yuv_config;
    bad.output.chroma_loc = HDRG_CHROMA_CENTER;
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, &caps, &bad, &session));
    bad = yuv_config;
    bad.output.matrix = HDRG_MATRIX_BT709;
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, &caps, &bad, &session));
    bad = yuv_config;
    bad.output.layout = HDRG_YUV_444;
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, &caps, &bad, &session));
    bad = sixteen;
    bad.output.bits = 10;
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, &caps, &bad, &session));
    bad = sixteen;
    bad.input.layout = HDRG_YUV_420;
    bad.input.matrix = HDRG_MATRIX_BT709;
    bad.input.bits = 7;
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, &caps, &bad, &session));
    bad.input.bits = 10;
    bad.input.chroma_loc = HDRG_CHROMA_BOTTOM + 1u;
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, &caps, &bad, &session));

    hdrg_context_destroy(ctx);
    free(rgb);
    free(expected);
    free(got);
}

/* one frame of format through a session of config on threads */
static void render_frame(const hdrg_raw_image_t *format, const hdrg_session_config_t *config,
                         uint8_t threads, const uint8_t *pixels, uint8_t *rows,
                         hdrg_session_output_t *info)
{
    hdrg_params_t params;
    hdrg_session_config_t local;
    hdrg_session_frame_t in;
    hdrg_session_t *session;
    hdrg_context_t *ctx;

    params_pq(&params, threads);
    local = *config;
    local.dither_off = true;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(format), &params, NULL,
                                                       &local, &session));

    memset(&in, 0, sizeof(in));
    in.planes[0] = pixels;
    memset(info, 0, sizeof(*info));
    info->planes[0] = rows;
    info->strides[0] = (size_t)format->width * 3u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, info));

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/*
 * A frame of twice the proxy side is analyzed on the center pixels of 2 x 2
 * squares: its statistics are the analysis of that reduction (16-bit codes),
 * its output does not depend on the thread count and stays near the output
 * of the full-resolution analysis
 */
static void test_proxy(void)
{
    const size_t row = (size_t)TS_BIG_W * 3u;
    hdrg_params_t params, effective;
    hdrg_encode_params_t encode;
    hdrg_raw_image_t format, reduced_format;
    hdrg_session_config_t config;
    hdrg_display_caps_t caps;
    hdrg_color_t color;
    hdrg_pipeline_t pipeline;
    hdrg_analysis_t analysis;
    hdrg_session_output_t one, four, full;
    hdrg_context_t *ctx;
    uint32_t x, y;
    uint16_t *reduced;
    uint8_t *image, *rows_one, *rows_four, *rows_full, c;
    size_t i;
    double diff;

    image = hdrg_test_image(TS_BIG_W, TS_BIG_H);
    reduced = (uint16_t *)malloc((size_t)TS_W * TS_H * 3u * sizeof(uint16_t));
    rows_one = (uint8_t *)malloc(row * TS_BIG_H);
    rows_four = (uint8_t *)malloc(row * TS_BIG_H);
    rows_full = (uint8_t *)malloc(row * TS_BIG_H);
    TEST_ASSERT_TRUE(image != NULL && reduced != NULL && rows_one != NULL && rows_four != NULL &&
                     rows_full != NULL);

    memset(&format, 0, sizeof(format));
    format.width = TS_BIG_W;
    format.height = TS_BIG_H;
    format.bit_depth = 8;
    format.stride = row;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    config = rgb_config(0, 0, 8);
    config.proxy = TS_W;

    render_frame(&format, &config, 1, image, rows_one, &one);
    render_frame(&format, &config, 4, image, rows_four, &four);
    TEST_ASSERT_EQUAL_MEMORY(rows_one, rows_four, row * TS_BIG_H);

    config.proxy = 0;
    render_frame(&format, &config, 1, image, rows_full, &full);

    for (y = 0; y < TS_H; y++) {
        for (x = 0; x < TS_W; x++) {
            for (c = 0; c < 3u; c++) {
                reduced[((size_t)y * TS_W + x) * 3u + c] =
                    (uint16_t)(image[((size_t)(2u * y + 1u) * TS_BIG_W + 2u * x + 1u) * 3u + c] *
                               257u);
            }
        }
    }

    params_pq(&params, 1);
    encode_pq(&encode, true);
    TEST_ASSERT_TRUE(hdrg_params_resolve(&params, &effective));
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_display_caps_init(&caps);
    reduced_format = format;
    reduced_format.width = TS_W;
    reduced_format.height = TS_H;
    reduced_format.bit_depth = 16;
    reduced_format.stride = (size_t)TS_W * 6u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_pipeline_open(ctx, &reduced_format, &color, &effective, &encode,
                                             &caps, 0, HDRG_LATENCY_OFFLINE, &pipeline));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_pipeline_analyze(&pipeline, (const uint8_t *)reduced, NULL, &analysis));
    TEST_ASSERT_TRUE(analysis.p995_luma == one.anchor_luma && analysis.key_luma == one.key_luma);
    TEST_ASSERT_TRUE(one.anchor_luma != full.anchor_luma || one.key_luma != full.key_luma);

    diff = 0.0;
    for (i = 0; i < row * TS_BIG_H; i++) {
        diff += fabs((double)rows_one[i] - (double)rows_full[i]);
    }

    TEST_ASSERT_TRUE(diff / (double)(row * TS_BIG_H) < 1.0);

    hdrg_analysis_free(&ctx->alloc, &analysis);
    hdrg_pipeline_close(ctx, &pipeline);
    hdrg_color_free(&ctx->alloc, &color);
    hdrg_context_destroy(ctx);
    free(image);
    free(reduced);
    free(rows_one);
    free(rows_four);
    free(rows_full);
}

/* the frames of sequence 0 through a session of latency's defaults and effort, undithered */
static void effort_rows(uint8_t latency, uint8_t effort, uint8_t *rows)
{
    const size_t stride = (size_t)TS_W * 3u;
    hdrg_session_config_t config;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t sent, received;

    hdrg_session_config_init(&config);
    config.latency = latency;
    config.dither_off = true;
    params_pq(&params, 1);
    params.effort = effort;
    format_of(&format);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));

    sent = 0;
    received = 0;
    for (;;) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows + (size_t)received * TS_H * stride;
        out.strides[0] = stride;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.planes[0] = frame_pixels(sent, 0);

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TS_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;
    }

    TEST_ASSERT_EQUAL_UINT32(TS_FRAMES, received);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/*
 * The defaults of a session: effort AUTO is QUALITY when
 * converting a file (OFFLINE), REALTIME when watched (PLAYBACK, LOW); the
 * statistics look as far ahead as back, LOW not ahead
 */
static void test_defaults(void)
{
    static const uint8_t latencies[2] = {HDRG_LATENCY_PLAYBACK, HDRG_LATENCY_LOW};
    const size_t bytes = (size_t)TS_W * TS_H * 3u * TS_FRAMES;
    hdrg_session_config_t config;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint8_t *got, *quality, *realtime, i;

    got = (uint8_t *)malloc(bytes);
    quality = (uint8_t *)malloc(bytes);
    realtime = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(got != NULL && quality != NULL && realtime != NULL);

    effort_rows(HDRG_LATENCY_OFFLINE, HDRG_EFFORT_QUALITY, quality);
    effort_rows(HDRG_LATENCY_OFFLINE, HDRG_EFFORT_REALTIME, realtime);
    TEST_ASSERT_TRUE(memcmp(quality, realtime, bytes) != 0);
    effort_rows(HDRG_LATENCY_OFFLINE, HDRG_EFFORT_AUTO, got);
    TEST_ASSERT_EQUAL_MEMORY(quality, got, bytes);

    for (i = 0; i < 2u; i++) {
        effort_rows(latencies[i], HDRG_EFFORT_REALTIME, realtime);
        effort_rows(latencies[i], HDRG_EFFORT_AUTO, got);
        TEST_ASSERT_EQUAL_MEMORY(realtime, got, bytes);
    }

    params_pq(&params, 1);
    format_of(&format);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));

    for (i = 0; i < 2u; i++) {
        hdrg_session_config_init(&config);
        config.latency = latencies[i];
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                           NULL, &config, &session));
        TEST_ASSERT_EQUAL_UINT32(
            (latencies[i] == HDRG_LATENCY_LOW || !HDRG_BUILTIN_STATS) ? 0u : HDRG_SESSION_PAST,
            hdrg_session_latency(session));
        hdrg_session_destroy(session);
    }

    hdrg_context_destroy(ctx);
    free(got);
    free(quality);
    free(realtime);
}

/*
 * Once the first frames are through (and the lights' history over the
 * window's past), a session takes no more memory from the context's
 * allocator: the frames' analyses (on a proxy here), plans and renders reuse
 * the blocks the earlier frames freed
 */
static void test_steady_allocations(void)
{
    const size_t stride = (size_t)TS_W * 3u;
    hdrg_allocator_t alloc;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t sent, received, warm;
    uint8_t *rows;

    alloc.malloc_fn = count_malloc;
    alloc.calloc_fn = NULL;
    alloc.realloc_fn = count_realloc;
    alloc.free_fn = count_free;
    alloc.user = NULL;
    params_pq(&params, 2);
    format_of(&format);
    config = rgb_config(TS_PAST, TS_FUTURE, 8);
    config.proxy = TS_W / 2u;
    rows = (uint8_t *)malloc(stride * TS_H);
    TEST_ASSERT_NOT_NULL(rows);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &alloc, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));

    sent = 0;
    received = 0;
    warm = 0;
    for (;;) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows;
        out.strides[0] = stride;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.planes[0] = frame_pixels(sent, TS_PAN);

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TS_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;

        if (received == TS_PAST + 4u) {
            warm = hdrg_atomic_u32_load_relaxed(&g_mallocs);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(TS_FRAMES, received);
    TEST_ASSERT_EQUAL_UINT32(warm, hdrg_atomic_u32_load_relaxed(&g_mallocs));

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(rows);
}

/*
 * ... and with the frames' work in the filters' bands: noisy frames through the
 * built-in NR, deblocking and debanding, upscaled twice by the SCALE stage's
 * Lanczos (each band worker's job_scratch kept by the host across the frames)
 */
static void test_steady_band_filters(void)
{
    const size_t stride = (size_t)TS_W * 2u * 3u;
    hdrg_allocator_t alloc;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t sent, received, warm;
    uint8_t *rows;

    HDRG_TEST_REQUIRE_BUILTIN(DENOISE);
    HDRG_TEST_REQUIRE_BUILTIN(DEBLOCK);
    HDRG_TEST_REQUIRE_BUILTIN(DEBAND);

    alloc.malloc_fn = count_malloc;
    alloc.calloc_fn = NULL;
    alloc.realloc_fn = count_realloc;
    alloc.free_fn = count_free;
    alloc.user = NULL;
    params_pq(&params, 4);
    params.grain_policy = HDRG_GRAIN_REDUCE;
    params.denoise_strength = 1.0f;
    params.deblock_strength = 1.0f;
    params.deband_strength = 1.0f;
    format_of(&format);
    config = rgb_config(TS_PAST, TS_FUTURE, 8);
    config.output_width = TS_W * 2u;
    config.output_height = TS_H * 2u;
    rows = (uint8_t *)malloc(stride * TS_H * 2u);
    TEST_ASSERT_NOT_NULL(rows);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &alloc, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));

    sent = 0;
    received = 0;
    warm = 0;
    for (;;) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows;
        out.strides[0] = stride;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.planes[0] = frame_pixels(sent, TS_NOISY);

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TS_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;

        if (received == TS_PAST + 4u) {
            warm = hdrg_atomic_u32_load_relaxed(&g_mallocs);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(TS_FRAMES, received);
    TEST_ASSERT_EQUAL_UINT32(warm, hdrg_atomic_u32_load_relaxed(&g_mallocs));

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(rows);
}

/*
 * RECURSIVE: the first frame of each scene renders on its own statistics
 * (the still pipeline's rows), the state sits on the 1/1024-stop grid, and a
 * highlight blinking every other frame pulls the anchor up faster than it
 * lets it down (toward the dimmer rendition): it settles above the middle
 */
static void test_recursive(void)
{
    const size_t stride = (size_t)TS_W * 3u;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t config, own;
    hdrg_session_output_t info[TS_FRAMES], raw[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t t;
    uint8_t *rows, *want;
    float steps, lo, hi;

    TS_REQUIRE_FILTER();

    rows = (uint8_t *)malloc(stride * TS_H * TS_FRAMES);
    want = (uint8_t *)malloc(stride * TS_H);
    TEST_ASSERT_TRUE(rows != NULL && want != NULL);

    config = rgb_config(0, 0, 8);
    config.temporal = HDRG_TEMPORAL_RECURSIVE;
    own = rgb_config(0, 0, 8);

    session_rows(&config, 1, TS_SCENE, stride, rows, info);
    still_rows(frame_pixels(0, TS_SCENE), want);
    TEST_ASSERT_EQUAL_MEMORY(want, rows, stride * TS_H);
    still_rows(frame_pixels(TS_CUT, TS_SCENE), want);
    TEST_ASSERT_EQUAL_MEMORY(want, rows + (size_t)TS_CUT * TS_H * stride, stride * TS_H);
    TEST_ASSERT_TRUE(info[TS_CUT].scene_cut);

    session_rows(&config, 1, TS_FLASH, stride, rows, info);
    session_rows(&own, 1, TS_FLASH, stride, rows, raw);

    for (t = 1; t < TS_FRAMES; t++) {
        steps = log2f(info[t].anchor_used) * 1024.0f;
        TEST_ASSERT_TRUE(fabsf(steps - roundf(steps)) < 0.01f);
    }

    lo = log2f(raw[TS_FRAMES - 2u].anchor_luma);
    hi = log2f(raw[TS_FRAMES - 1u].anchor_luma);
    TEST_ASSERT_TRUE(hi > lo + 1.0f);
    TEST_ASSERT_TRUE(log2f(info[TS_FRAMES - 2u].anchor_used) > lo + 0.6f * (hi - lo));

    /* no future window */
    config.future = 1;
    params_pq(&params, 1);
    format_of(&format);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, NULL, &config, &session));
    hdrg_context_destroy(ctx);

    free(rows);
    free(want);
}

/*
 * TWO_PASS: the first pass gives each frame's statistics and APL factor
 * without rendering; the second renders every frame of a scene on the
 * scene's log-mean statistics, keeps the first pass's cuts and refuses
 * frames past them. A second pass started in the middle (a seek) renders
 * its frames byte for byte as the whole pass
 */
static void test_two_pass(void)
{
    const size_t stride = (size_t)TS_W * 3u;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t config, bad;
    hdrg_session_pass_t passes[TS_FRAMES], broken[TS_FRAMES];
    hdrg_session_light_t wrong;
    hdrg_session_frame_t in;
    hdrg_session_output_t out, info[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t t, sent, received, start;
    uint8_t *rows, *seek;
    double sum;
    float want;

    TS_REQUIRE_FILTER();

    rows = (uint8_t *)malloc(stride * TS_H * TS_FRAMES);
    TEST_ASSERT_NOT_NULL(rows);

    /* a budget that binds in the first scene by a factor that changes frame to frame */
    g_apl_budget = 92.0f;
    params_pq(&params, 1);
    format_of(&format);
    config = rgb_config(8, 0, 8);
    config.temporal = HDRG_TEMPORAL_TWO_PASS;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));

    g_kept = 0;
    sent = 0;
    received = 0;
    for (;;) {
        memset(&out, 0, sizeof(out));

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.planes[0] = frame_pixels(sent, TS_SCENE);
            in.pts = (int64_t)sent;

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TS_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(out.pts == (int64_t)received && out.apl_nits == 0.0f);
        TEST_ASSERT_TRUE(out.apl_fit >= 0.0f && out.apl_fit <= 1.0f);
        passes[received].lights = keep_lights(&out);
        passes[received].light_count = out.light_count;
        passes[received].anchor_luma = out.anchor_luma;
        passes[received].key_luma = out.key_luma;
        passes[received].apl_fit = out.apl_fit;
        passes[received].scene_cut = out.scene_cut;
        received++;
    }

    hdrg_session_destroy(session);
    TEST_ASSERT_EQUAL_UINT32(TS_FRAMES, received);

    for (t = 0; t < TS_FRAMES; t++) {
        TEST_ASSERT_TRUE(passes[t].scene_cut == (t == 0 || t == TS_CUT));
        TEST_ASSERT_TRUE(t >= TS_CUT || (passes[t].apl_fit > 0.0f && passes[t].apl_fit < 1.0f));
    }

    config.passes = passes;
    config.pass_count = TS_FRAMES;
    session_rows(&config, 1, TS_SCENE, stride, rows, info);

    for (start = 0; start < TS_FRAMES; start = TS_CUT) {
        sum = 0.0;
        for (t = start; t < ((start == 0) ? TS_CUT : TS_FRAMES); t++) {
            sum += (double)hdrg_math_log2f(passes[t].anchor_luma);
        }

        want = hdrg_math_exp2f((float)(sum / (double)(t - start)));

        for (t = start; t < ((start == 0) ? TS_CUT : TS_FRAMES); t++) {
            TEST_ASSERT_TRUE(info[t].anchor_used == want);
            TEST_ASSERT_TRUE(info[t].scene_cut == (t == start));
        }

        if (start == TS_CUT) {
            break;
        }
    }

    /* a second pass from frame TS_SEEK on renders those frames as the whole pass did */
    config.start = TS_SEEK;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));
    seek = (uint8_t *)malloc(stride * TS_H);
    TEST_ASSERT_NOT_NULL(seek);

    for (t = TS_SEEK; t < TS_FRAMES; t++) {
        memset(&in, 0, sizeof(in));
        in.planes[0] = frame_pixels(t, TS_SCENE);
        memset(&out, 0, sizeof(out));
        out.planes[0] = seek;
        out.strides[0] = stride;

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &out));
        TEST_ASSERT_EQUAL_MEMORY(rows + (size_t)t * TS_H * stride, seek, stride * TS_H);
        TEST_ASSERT_TRUE(out.anchor_used == info[t].anchor_used &&
                         out.apl_scale == info[t].apl_scale);
    }

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE, hdrg_session_send_frame(session, &in));
    hdrg_session_destroy(session);
    free(seek);
    config.start = 0;

    /* frames past the first pass's, a future window and broken configs are refused */
    config.pass_count = 2;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));
    memset(&in, 0, sizeof(in));
    in.planes[0] = frame_pixels(0, TS_SCENE);
    memset(&out, 0, sizeof(out));
    out.planes[0] = rows;
    out.strides[0] = stride;

    for (t = 0; t < 2u; t++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &out));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE, hdrg_session_send_frame(session, &in));
    hdrg_session_destroy(session);

    bad = config;
    bad.future = 2;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    bad = config;
    bad.pass_count = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    bad = config;
    bad.start = TS_FRAMES;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    bad = config;
    bad.passes = NULL;
    bad.start = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    bad = config;
    bad.temporal = 9;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));

    /* a light out of the frame, of a boost past 1 or missing */
    memcpy(broken, passes, sizeof(broken));
    bad = config;
    bad.passes = broken;
    wrong.x0 = 2;
    wrong.y0 = 2;
    wrong.x1 = TS_W;
    wrong.y1 = 4;
    wrong.boost = 0.5f;
    broken[1].lights = &wrong;
    broken[1].light_count = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    wrong.x1 = 4;
    wrong.boost = 1.5f;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    broken[1].lights = NULL;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));
    wrong.boost = 0.5f;
    broken[1].lights = &wrong;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &bad, &session));
    hdrg_session_destroy(session);
    bad = config;
    bad.rate_num = 30;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format),
                                                                    &params, NULL, &bad, &session));

    g_apl_budget = 0.0f;
    hdrg_context_destroy(ctx);
    free(rows);
}

/* a build without the temporal filters refuses a window and the filtering modes */
static void test_filters_off(void)
{
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint8_t mode;

#if HDRG_BUILTIN_STATS
    TEST_IGNORE_MESSAGE("built with the temporal filters");
#endif

    params_pq(&params, 1);
    format_of(&format);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));

    for (mode = 0; mode < 4u; mode++) {
        config = rgb_config((mode == 0) ? 1u : 0u, (mode == 1) ? 1u : 0u, 8);
        config.temporal = (mode < 2u)
                              ? HDRG_TEMPORAL_WINDOWED
                              : ((mode == 2u) ? HDRG_TEMPORAL_RECURSIVE : HDRG_TEMPORAL_TWO_PASS);
        TEST_ASSERT_EQUAL_INT(
            HDRG_ERR_UNSUPPORTED,
            hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params, NULL, &config, &session));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, NULL, &session));
    TEST_ASSERT_EQUAL_UINT32(0, hdrg_session_latency(session));
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* one 16-bit frame (TS_W x TS_H) through a STATIC session on caps into rows (16-bit PQ) */
static void render_static(const hdrg_display_caps_t *caps, const uint16_t *pixels, uint16_t *rows)
{
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;

    params_pq(&params, 2);
    format_of(&format);
    format.bit_depth = 16;
    format.stride = (size_t)TS_W * 6u;
    config = rgb_config(0, 0, 16);
    config.temporal = HDRG_TEMPORAL_STATIC;
    config.dither_off = true;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       caps, &config, &session));

    memset(&in, 0, sizeof(in));
    in.planes[0] = (const uint8_t *)(const void *)pixels;
    memset(&out, 0, sizeof(out));
    out.planes[0] = (uint8_t *)(void *)rows;
    out.strides[0] = (size_t)TS_W * 6u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_receive_frame(session, &out));
    TEST_ASSERT_TRUE(out.scene_cut && out.apl_scale == 1.0f && out.max_nits <= out.peak_nits);
    TEST_ASSERT_TRUE(out.noise_sigma < 0.0f && out.blockiness == 1.0f);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/*
 * STATIC: every pixel takes a fixed mapping of its own code values (two
 * frames of the pan, five pixels apart, give the same rows five pixels
 * apart), which hdrg_lut3d_export samples: a frame of the LUT's node codes
 * renders as the LUT says; the neutral axis rises
 */
static void test_static(void)
{
    const size_t samples = (size_t)TS_W * TS_H * 3u, nodes = 17u * 17u * 17u;
    hdrg_params_t params, effective, prefiltered;
    hdrg_display_caps_t caps;
    hdrg_context_t *ctx;
    uint32_t x, y, step[3];
    uint16_t *frame0, *frame5, *rows0, *rows5;
    uint8_t c;
    size_t k;
    float *lut, *other;

    frame0 = (uint16_t *)malloc(samples * sizeof(uint16_t));
    frame5 = (uint16_t *)malloc(samples * sizeof(uint16_t));
    rows0 = (uint16_t *)malloc(samples * sizeof(uint16_t));
    rows5 = (uint16_t *)malloc(samples * sizeof(uint16_t));
    lut = (float *)malloc(nodes * 3u * sizeof(float));
    other = (float *)malloc(nodes * 3u * sizeof(float));
    TEST_ASSERT_TRUE(frame0 != NULL && frame5 != NULL && rows0 != NULL && rows5 != NULL &&
                     lut != NULL && other != NULL);

    params_pq(&params, 1);
    TEST_ASSERT_TRUE(hdrg_params_resolve(&params, &effective));
    hdrg_display_caps_init(&caps);

    for (y = 0; y < TS_H; y++) {
        for (x = 0; x < TS_W * 3u; x++) {
            frame0[(size_t)y * TS_W * 3u + x] =
                (uint16_t)(frame_pixels(0, TS_PAN)[(size_t)y * (TS_W + TS_FRAMES) * 3u + x] * 257u);
            frame5[(size_t)y * TS_W * 3u + x] =
                (uint16_t)(frame_pixels(5, TS_PAN)[(size_t)y * (TS_W + TS_FRAMES) * 3u + x] * 257u);
        }
    }

    render_static(&caps, frame0, rows0);
    render_static(&caps, frame5, rows5);

    for (y = 0; y < TS_H; y++) {
        TEST_ASSERT_EQUAL_MEMORY(rows0 + ((size_t)y * TS_W + 5u) * 3u,
                                 rows5 + (size_t)y * TS_W * 3u,
                                 (size_t)(TS_W - 5u) * 3u * sizeof(uint16_t));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_lut3d_export(ctx, HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB,
                                                     &params, &caps, 17, lut));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_lut3d_export(ctx, HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &params, &caps, 1, lut));

    /* the LUT is the color mapping alone: the params' PREFILTER stays out, an added one is refused
     */
    prefiltered = params;
    prefiltered.deblock_strength = 1.0f;
    prefiltered.dering_strength = 1.0f;
    prefiltered.grain_policy = HDRG_GRAIN_REDUCE;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_lut3d_export(ctx, HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB,
                                                     &prefiltered, &caps, 17, other));
    TEST_ASSERT_EQUAL_MEMORY(lut, other, nodes * 3u * sizeof(float));
    TEST_ASSERT_EQUAL_INT((HDRG_BUILTIN_DEBLOCK != 0) ? HDRG_OK : HDRG_ERR_NOT_FOUND,
                          hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, "hdrg.deblock", NULL));
    TEST_ASSERT_EQUAL_INT((HDRG_BUILTIN_DEBLOCK != 0) ? HDRG_ERR_UNSUPPORTED : HDRG_OK,
                          hdrg_lut3d_export(ctx, HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &params,
                                            &caps, 17, other));
    hdrg_context_destroy(ctx);

    memset(frame0, 0, samples * sizeof(uint16_t));

    for (k = 0; k < nodes; k++) {
        step[0] = (uint32_t)(k % 17u);
        step[1] = (uint32_t)((k / 17u) % 17u);
        step[2] = (uint32_t)(k / 289u);

        for (c = 0; c < 3u; c++) {
            frame0[k * 3u + c] = (uint16_t)((step[c] * 65535u + 8u) / 16u);
        }
    }

    render_static(&caps, frame0, rows0);

    for (k = 0; k < nodes * 3u; k++) {
        TEST_ASSERT_EQUAL_UINT16(rows0[k], (uint16_t)(lut[k] * 65535.0f + 0.5f));
    }

    for (k = 1; k < 17u; k++) {
        TEST_ASSERT_TRUE(lut[(k * 289u + k * 17u + k) * 3u] >
                         lut[((k - 1u) * 289u + (k - 1u) * 17u + k - 1u) * 3u]);
    }

    free(frame0);
    free(frame5);
    free(rows0);
    free(rows5);
    free(lut);
    free(other);
}

/*
 * Frames of TS_W x TS_H through a session, alternating between two: code
 * low, the top three fifths at code even / odd, and an 18 x 18 white square
 * at the bottom (2% of the frame) that holds the scene anchor at white. No
 * APL budget: it would hold the average level of the frames together itself
 */
static void flash_run(uint8_t low, uint8_t even, uint8_t odd, bool off, hdrg_session_output_t *info)
{
    const size_t row = (size_t)TS_W * 3u, offsets[3] = {0, 0, 0}, strides[3] = {row, 0, 0};
    hdrg_params_t params;
    hdrg_session_format_t format;
    hdrg_session_config_t config;
    hdrg_session_frame_t frames[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t t;
    uint8_t *images, *rows;
    size_t i;

    images = (uint8_t *)malloc(row * TS_H * 2u);
    rows = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    TEST_ASSERT_TRUE(images != NULL && rows != NULL);

    for (i = 0; i < row * TS_H; i++) {
        images[i] = (i < row * (TS_H * 3u / 5u)) ? even : low;
        images[row * TS_H + i] = (i < row * (TS_H * 3u / 5u)) ? odd : low;
    }

    for (i = 0; i < 18u * 18u; i++) {
        memset(images + ((TS_H - 24u + i / 18u) * TS_W + 10u + i % 18u) * 3u, 255, 3);
        memset(images + row * TS_H + ((TS_H - 24u + i / 18u) * TS_W + 10u + i % 18u) * 3u, 255, 3);
    }

    memset(frames, 0, sizeof(frames));

    for (t = 0; t < TS_FRAMES; t++) {
        frames[t].planes[0] = images + (size_t)(t % 2u) * row * TS_H;
        frames[t].pts = (int64_t)t;
    }

    params_pq(&params, 1);
    params.apl_budget_nits = 0.0f;
    params.apl_peak_fraction = 0.0f;
    memset(&format, 0, sizeof(format));
    format.width = TS_W;
    format.height = TS_H;
    format.bit_depth = 8;
    format.stride = row;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    config = rgb_config(0, 0, 8);
    config.flash_off = off;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &config, &session));
    drive(session, frames, 1, offsets, strides, row * TS_H, rows, info);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    free(images);
    free(rows);
}

/*
 * The flash limiter (BT.1702-3): six harmful changes a second pass; from the
 * seventh the rises are limited (a lower gain does not soften a fall). A
 * strobe already harmful in the source rises with no gain; a rise only the
 * gain makes harmful (codes 205 and 215 in the bright region: 14 cd/m^2
 * apart at SDR white) keeps part of it. flash_off limits nothing
 */
static void test_flash(void)
{
    hdrg_session_output_t limited[TS_FRAMES], free_run[TS_FRAMES];
    uint32_t t;

    flash_run(20, 20, 200, false, limited);
    flash_run(20, 20, 200, true, free_run);

    for (t = 0; t < TS_FRAMES; t++) {
        TEST_ASSERT_TRUE(limited[t].flash_limited == (t >= 7u && t % 2u == 1u));
        TEST_ASSERT_FALSE(free_run[t].flash_limited);

        if (limited[t].flash_limited) {
            TEST_ASSERT_TRUE(limited[t].apl_scale == 0.0f);
        }
    }

    TEST_ASSERT_TRUE(free_run[TS_FRAMES - 1u].apl_scale > 0.0f);

    flash_run(20, 205, 215, false, limited);
    flash_run(20, 205, 215, true, free_run);

    for (t = 0; t < TS_FRAMES; t++) {
        TEST_ASSERT_TRUE(limited[t].flash_limited == (t >= 7u && t % 2u == 1u));

        if (limited[t].flash_limited) {
            TEST_ASSERT_TRUE(limited[t].apl_scale > 0.0f &&
                             limited[t].apl_scale < free_run[t].apl_scale);
        }
    }
}

/* the session's statistics of one frame of format (zero window) */
static void session_stats(const hdrg_raw_image_t *format, const uint8_t *pixels,
                          hdrg_session_output_t *info)
{
    const hdrg_session_config_t config = rgb_config(0, 0, 8);
    uint8_t *rows;

    rows = (uint8_t *)malloc((size_t)format->width * 3u * format->height);
    TEST_ASSERT_NOT_NULL(rows);

    render_frame(format, &config, 1, pixels, rows, info);
    free(rows);
}

/* the analysis statistics of pixels of format, with no black bars left out */
static void plain_stats(const hdrg_raw_image_t *format, const uint8_t *pixels, float *p995,
                        float *key)
{
    hdrg_params_t params, effective;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t caps;
    hdrg_color_t color;
    hdrg_pipeline_t pipeline;
    hdrg_analysis_t analysis;
    hdrg_context_t *ctx;

    params_pq(&params, 1);
    encode_pq(&encode, false);
    TEST_ASSERT_TRUE(hdrg_params_resolve(&params, &effective));
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_display_caps_init(&caps);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_open(ctx, format, &color, &effective, &encode,
                                                      &caps, 0, HDRG_LATENCY_OFFLINE, &pipeline));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_analyze(&pipeline, pixels, NULL, &analysis));
    *p995 = analysis.p995_luma;
    *key = analysis.key_luma;

    hdrg_analysis_free(&ctx->alloc, &analysis);
    hdrg_pipeline_close(ctx, &pipeline);
    hdrg_color_free(&ctx->alloc, &color);
    hdrg_context_destroy(ctx);
}

/*
 * Black bars at the edges (12 rows top and bottom; 20 columns left and
 * right) are left out of a session's statistics: they equal those of the
 * picture inside alone. A black frame keeps every pixel (no bars)
 */
static void test_letterbox(void)
{
    hdrg_raw_image_t format, inner;
    hdrg_session_output_t info;
    uint32_t y;
    uint8_t *picture, *framed;
    float p995, key;

    picture = hdrg_test_image(TS_W, TS_H - 24u);
    framed = (uint8_t *)calloc((size_t)TS_W * TS_H, 3u);
    TEST_ASSERT_TRUE(picture != NULL && framed != NULL);

    memset(&format, 0, sizeof(format));
    format.width = TS_W;
    format.height = TS_H;
    format.bit_depth = 8;
    format.stride = (size_t)TS_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    inner = format;
    inner.height = TS_H - 24u;

    memcpy(framed + (size_t)12u * TS_W * 3u, picture, (size_t)TS_W * 3u * (TS_H - 24u));
    session_stats(&format, framed, &info);
    plain_stats(&inner, picture, &p995, &key);
    TEST_ASSERT_TRUE(info.anchor_luma == p995 && info.key_luma == key);
    plain_stats(&format, framed, &p995, &key);
    TEST_ASSERT_TRUE(info.key_luma > key);

    free(picture);
    picture = hdrg_test_image(TS_W - 40u, TS_H);
    TEST_ASSERT_NOT_NULL(picture);
    memset(framed, 0, (size_t)TS_W * TS_H * 3u);

    for (y = 0; y < TS_H; y++) {
        memcpy(framed + ((size_t)y * TS_W + 20u) * 3u, picture + (size_t)y * (TS_W - 40u) * 3u,
               (size_t)(TS_W - 40u) * 3u);
    }

    inner = format;
    inner.width = TS_W - 40u;
    inner.stride = (size_t)(TS_W - 40u) * 3u;
    session_stats(&format, framed, &info);
    plain_stats(&inner, picture, &p995, &key);
    TEST_ASSERT_TRUE(info.anchor_luma == p995 && info.key_luma == key);

    memset(framed, 0, (size_t)TS_W * TS_H * 3u);
    session_stats(&format, framed, &info);
    plain_stats(&format, framed, &p995, &key);
    TEST_ASSERT_TRUE(info.anchor_luma == p995 && info.key_luma == key);

    free(picture);
    free(framed);
}

/*
 * A fade-out (codes x 0.95 a frame from frame 6): from its third frame the
 * frames hold the anchor statistics and the APL factor of frame 5, and the
 * output falls monotonically; a zero window holds nothing
 */
static void test_fade(void)
{
    const size_t row = (size_t)TS_W * 3u, offsets[3] = {0, 0, 0}, strides[3] = {row, 0, 0};
    hdrg_params_t params;
    hdrg_session_format_t format;
    hdrg_session_config_t config;
    hdrg_session_frame_t frames[TS_FRAMES];
    hdrg_session_output_t info[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t t;
    uint8_t *picture, *images, *rows, round;
    size_t i;
    float k;

    TS_REQUIRE_FILTER();

    picture = hdrg_test_image(TS_W, TS_H);
    images = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    rows = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    TEST_ASSERT_TRUE(picture != NULL && images != NULL && rows != NULL);

    memset(frames, 0, sizeof(frames));
    k = 1.0f;

    for (t = 0; t < TS_FRAMES; t++) {
        k = (t > 5u) ? k * 0.95f : 1.0f;

        for (i = 0; i < row * TS_H; i++) {
            images[(size_t)t * row * TS_H + i] = (uint8_t)((float)picture[i] * k + 0.5f);
        }

        frames[t].planes[0] = images + (size_t)t * row * TS_H;
        frames[t].pts = (int64_t)t;
    }

    params_pq(&params, 1);
    memset(&format, 0, sizeof(format));
    format.width = TS_W;
    format.height = TS_H;
    format.bit_depth = 8;
    format.stride = row;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));

    for (round = 0; round < 2u; round++) {
        config = rgb_config((round == 0) ? 8u : 0u, 0, 8);
        config.dither_off = true;
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_session_create(ctx, &format, &params, NULL, &config, &session));
        drive(session, frames, 1, offsets, strides, row * TS_H, rows, info);
        hdrg_session_destroy(session);

        for (t = 0; t < TS_FRAMES; t++) {
            TEST_ASSERT_FALSE(info[t].scene_cut && t > 0u);
            TEST_ASSERT_TRUE(info[t].fading == (round == 0 && t >= 8u));

            if (info[t].fading) {
                TEST_ASSERT_TRUE(info[t].anchor_used == info[5].anchor_used &&
                                 info[t].apl_scale == info[5].apl_scale);
            }

            if (round == 0 && t > 5u) {
                TEST_ASSERT_TRUE(info[t].avg_nits < info[t - 1u].avg_nits);
            }
        }
    }

    hdrg_context_destroy(ctx);
    free(picture);
    free(images);
    free(rows);
}

/*
 * A fade to black (codes falling linearly from frame 6 to none at the last
 * frame): near black a step moves the luminance a stop or more, yet no frame
 * after the first starts a scene, the output's brightest and mean levels
 * never rise (the highlight does not flash back up) and the mean falls in
 * proportion to the input's, within 5% while the codes are above a quarter.
 * WINDOWED: the frames from the third darkening one hold frame 5's
 * statistics through the black one. TWO_PASS (its statistics constant over
 * the scene) holds nothing, and its scene's statistics leave the fade out:
 * the frames before it render as WINDOWED's, within 1%
 */
static void test_fade_to_black(void)
{
    const size_t row = (size_t)TS_W * 3u, offsets[3] = {0, 0, 0}, strides[3] = {row, 0, 0};
    hdrg_params_t params;
    hdrg_session_format_t format;
    hdrg_session_config_t config;
    hdrg_session_frame_t frames[TS_FRAMES];
    hdrg_session_output_t info[TS_FRAMES];
    hdrg_session_pass_t passes[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t t;
    uint8_t *picture, *images, *rows, round;
    size_t i;
    float k, ratio, windowed;

    TS_REQUIRE_FILTER();

    picture = hdrg_test_image(TS_W, TS_H);
    images = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    rows = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    TEST_ASSERT_TRUE(picture != NULL && images != NULL && rows != NULL);

    memset(frames, 0, sizeof(frames));

    for (t = 0; t < TS_FRAMES; t++) {
        k = (t > 5u) ? 1.0f - (float)(t - 5u) / (float)(TS_FRAMES - 6u) : 1.0f;

        for (i = 0; i < row * TS_H; i++) {
            images[(size_t)t * row * TS_H + i] = (uint8_t)((float)picture[i] * k + 0.5f);
        }

        frames[t].planes[0] = images + (size_t)t * row * TS_H;
        frames[t].pts = (int64_t)t;
    }

    params_pq(&params, 1);
    memset(&format, 0, sizeof(format));
    format.width = TS_W;
    format.height = TS_H;
    format.bit_depth = 8;
    format.stride = row;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    windowed = 0.0f;

    for (round = 0; round < 2u; round++) {
        config = rgb_config(8, 0, 8);
        config.dither_off = true;

        /* TWO_PASS: the first pass's statistics */
        if (round == 1u) {
            config.temporal = HDRG_TEMPORAL_TWO_PASS;
            g_kept = 0;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_create(ctx, &format, &params, NULL, &config, &session));
            drive(session, frames, 1, offsets, strides, row * TS_H, rows, info);
            hdrg_session_destroy(session);

            for (t = 0; t < TS_FRAMES; t++) {
                passes[t].lights = info[t].lights;
                passes[t].light_count = info[t].light_count;
                passes[t].anchor_luma = info[t].anchor_luma;
                passes[t].key_luma = info[t].key_luma;
                passes[t].apl_fit = info[t].apl_fit;
                passes[t].scene_cut = info[t].scene_cut;
            }

            config.passes = passes;
            config.pass_count = TS_FRAMES;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_session_create(ctx, &format, &params, NULL, &config, &session));
        drive(session, frames, 1, offsets, strides, row * TS_H, rows, info);
        hdrg_session_destroy(session);

        ratio = info[5].avg_nits / info[5].sdr_apl_nits;
        windowed = (round == 0) ? ratio : windowed;
        TEST_ASSERT_TRUE(fabsf(ratio / windowed - 1.0f) <= 0.01f);

        for (t = 1; t < TS_FRAMES; t++) {
            k = (t > 5u) ? 1.0f - (float)(t - 5u) / (float)(TS_FRAMES - 6u) : 1.0f;
            TEST_ASSERT_FALSE(info[t].scene_cut);
            TEST_ASSERT_TRUE(info[t].fading == (round == 0 && t >= 8u));

            if (info[t].fading) {
                TEST_ASSERT_TRUE(info[t].anchor_used == info[5].anchor_used);
            }

            if (t > 5u) {
                TEST_ASSERT_TRUE(info[t].max_nits <= info[t - 1u].max_nits);
                TEST_ASSERT_TRUE(info[t].avg_nits <= info[t - 1u].avg_nits);
            }

            if (k >= 0.25f) {
                TEST_ASSERT_TRUE(fabsf(info[t].avg_nits / info[t].sdr_apl_nits / ratio - 1.0f) <=
                                 0.05f);
            }
        }
    }

    hdrg_context_destroy(ctx);
    free(picture);
    free(images);
    free(rows);
}

/*
 * PREFILTER (the grain policy REDUCE): without a window
 * each frame is the still pipeline's; with a future window (a ring of the
 * stage's outputs) the session equals one fed those outputs under
 * PRESERVE; Y'CbCr input (converted before the stage) equals its R'G'B'
 * conversion; the noise reported is the input's
 */
static void test_prefilter(void)
{
    const hdrg_session_pixfmt_t yuv709 = {
        HDRG_YUV_420, HDRG_MATRIX_BT709, 10, HDRG_CHROMA_LEFT, false, false, false};
    const size_t row = (size_t)TS_W * 3u, rgb_offsets[3] = {0, 0, 0}, rgb_strides[3] = {row, 0, 0};
    hdrg_params_t params, effective;
    hdrg_encode_params_t encode;
    hdrg_display_caps_t caps;
    hdrg_raw_image_t format;
    hdrg_color_t color;
    hdrg_pipeline_t pipeline;
    hdrg_pipeline_out_t out;
    hdrg_session_config_t window, yuv_config;
    hdrg_session_output_t info[TS_FRAMES];
    hdrg_session_frame_t frames[TS_FRAMES], yuv_frames[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_yuv_t yuv;
    uint32_t t, index;
    uint16_t *filtered, *wide;
    uint8_t *rows, *want, *got, *planes, p;
    size_t offsets[3], bytes;
    float *scratch;

    HDRG_TEST_REQUIRE_BUILTIN(DEBLOCK);
    HDRG_TEST_REQUIRE_BUILTIN(DENOISE);

    bytes = yuv420_at(NULL, HDRG_MATRIX_BT709, 10, &yuv, offsets);
    scratch = (float *)malloc(hdrg_yuv_scratch(&yuv) * sizeof(float));
    TEST_ASSERT_NOT_NULL(scratch);
    rows = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    want = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    got = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    filtered = (uint16_t *)malloc(row * TS_H * TS_FRAMES * sizeof(uint16_t));
    wide = (uint16_t *)malloc(row * TS_H * sizeof(uint16_t));
    planes = (uint8_t *)malloc(bytes * TS_FRAMES);
    TEST_ASSERT_TRUE(rows != NULL && want != NULL && got != NULL && filtered != NULL &&
                     wide != NULL && planes != NULL);
    g_grain = HDRG_GRAIN_REDUCE;

    /* no window, the analysis at full resolution */
    window = rgb_config(0, 0, 8);
    window.proxy = TS_W;
    session_rows(&window, 1, TS_NOISY, row, rows, info);

    for (t = 0; t < TS_FRAMES; t += 5) {
        still_rows(frame_pixels(t, TS_NOISY), want);
        TEST_ASSERT_EQUAL_MEMORY(want, rows + (size_t)t * TS_H * row, row * TS_H);
        TEST_ASSERT_TRUE(info[t].noise_sigma > 1.0f);
    }

    /* a future window: the stage's outputs wait in the ring */
    window = rgb_config(TS_PAST, TS_FUTURE, 8);
    window.proxy = TS_W;
    session_rows(&window, 3, TS_NOISY, row, rows, NULL);

    params_pq(&params, 1);
    encode_pq(&encode, true);
    TEST_ASSERT_TRUE(hdrg_params_resolve(&params, &effective));
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    hdrg_display_caps_init(&caps);
    format_of(&format);
    memset(frames, 0, sizeof(frames));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pipeline_open(ctx, &format, &color, &effective, &encode,
                                                      &caps, 0, HDRG_LATENCY_OFFLINE, &pipeline));
    TEST_ASSERT_TRUE(pipeline.prefiltering);

    for (t = 0; t < TS_FRAMES; t++) {
        out.dst = filtered + (size_t)t * row * TS_H;
        out.slot = UINT32_MAX;
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_pipeline_prefilter(ctx, &pipeline, frame_pixels(t, TS_NOISY),
                                                      format.stride, (int64_t)t, &out, &index));
        TEST_ASSERT_EQUAL_UINT32(t, index);
        TEST_ASSERT_TRUE(out.pixels == (const uint8_t *)(const void *)out.dst);
        frames[t].planes[0] = (const uint8_t *)(const void *)(filtered + (size_t)t * row * TS_H);
        frames[t].pts = (int64_t)t;
    }

    hdrg_pipeline_close(ctx, &pipeline);
    hdrg_color_free(&ctx->alloc, &color);

    g_grain = HDRG_GRAIN_PRESERVE;
    params_pq(&params, 3);
    format.bit_depth = 16;
    format.stride = row * 2u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &window, &session));
    drive(session, frames, 1, rgb_offsets, rgb_strides, row * TS_H, got, NULL);
    hdrg_session_destroy(session);
    TEST_ASSERT_EQUAL_MEMORY(rows, got, row * TS_H * TS_FRAMES);

    /* Y'CbCr input against its R'G'B' conversion, both through the stage */
    g_grain = HDRG_GRAIN_REDUCE;
    params_pq(&params, 2);
    memset(yuv_frames, 0, sizeof(yuv_frames));

    for (t = 0; t < TS_FRAMES; t++) {
        sequence_rgb16(t, TS_NOISY, wide);
        (void)yuv420_at(planes + (size_t)t * bytes, HDRG_MATRIX_BT709, 10, &yuv, offsets);
        hdrg_rgb16_to_yuv420(NULL, wide, 0, TS_H, &yuv);
        hdrg_yuv_to_rgb16(NULL, &yuv, scratch, filtered + (size_t)t * row * TS_H,
                          (size_t)yuv.width * 6u);

        for (p = 0; p < 3u; p++) {
            yuv_frames[t].planes[p] = (const uint8_t *)yuv.planes[p];
            yuv_frames[t].strides[p] = yuv.strides[p];
        }

        yuv_frames[t].pts = (int64_t)t;
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &window, &session));
    drive(session, frames, 1, rgb_offsets, rgb_strides, row * TS_H, want, NULL);
    hdrg_session_destroy(session);

    yuv_config = window;
    yuv_config.input = yuv709;
    format.bit_depth = 0;
    format.stride = 0;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &yuv_config, &session));
    drive(session, yuv_frames, 1, rgb_offsets, rgb_strides, row * TS_H, got, NULL);
    hdrg_session_destroy(session);
    TEST_ASSERT_EQUAL_MEMORY(want, got, row * TS_H * TS_FRAMES);

    g_grain = HDRG_GRAIN_PRESERVE;
    hdrg_context_destroy(ctx);
    free(rows);
    free(want);
    free(got);
    free(filtered);
    free(wide);
    free(planes);
    free(scratch);
}

/*
 * A session whose PREFILTER has nothing to do (deblocking clean frames) keeps
 * the stage's path for the whole sequence: each frame renders as given in
 * 16 bits (codes x 257) to a session without the stage
 */
static void test_prefilter_idle(void)
{
    const size_t row = (size_t)TS_W * 3u, rgb_offsets[3] = {0, 0, 0}, rgb_strides[3] = {row, 0, 0};
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_config_t window;
    hdrg_session_frame_t frames[TS_FRAMES];
    hdrg_session_output_t info[TS_FRAMES];
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t t;
    uint16_t *wide;
    uint8_t *rows, *got;

    HDRG_TEST_REQUIRE_BUILTIN(DEBLOCK);

    rows = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    got = (uint8_t *)malloc(row * TS_H * TS_FRAMES);
    wide = (uint16_t *)malloc(row * TS_H * TS_FRAMES * sizeof(uint16_t));
    TEST_ASSERT_TRUE(rows != NULL && got != NULL && wide != NULL);

    g_deblock = 1.0f;
    window = rgb_config(0, 0, 8);
    session_rows(&window, 1, TS_PAN, row, rows, info);
    g_deblock = 0.0f;
    memset(frames, 0, sizeof(frames));

    for (t = 0; t < TS_FRAMES; t++) {
        TEST_ASSERT_TRUE(info[t].blockiness <= 1.1f);
        sequence_rgb16(t, TS_PAN, wide + (size_t)t * row * TS_H);
        frames[t].planes[0] = (const uint8_t *)(const void *)(wide + (size_t)t * row * TS_H);
        frames[t].pts = (int64_t)t;
    }

    params_pq(&params, 1);
    format_of(&format);
    format.bit_depth = 16;
    format.stride = row * 2u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &window, &session));
    drive(session, frames, 1, rgb_offsets, rgb_strides, row * TS_H, got, NULL);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(rows, got, row * TS_H * TS_FRAMES);

    free(rows);
    free(got);
    free(wide);
}

/* frame t of the seek sequence into dst (rows of TS_SEEK_W) */
static void seek_frame(uint32_t t, uint8_t *dst)
{
    const uint8_t *src;
    const size_t row = (size_t)TS_SEEK_W * 3u, wide = (size_t)(TS_W + TS_FRAMES) * 3u;
    uint32_t y, shift, darkening;
    uint8_t top;
    size_t i;
    float k;

    /* the strobe's frames: the top three fifths at 20 or 200, the rest at 20, a white square */
    if ((t >= 80u && t < 114u) || t == 120u) {
        top = (t == 120u || (t < 110u && t % 2u == 1u)) ? 200u : 20u;

        for (i = 0; i < row * TS_SEEK_H; i++) {
            dst[i] = (i < row * (TS_SEEK_H * 3u / 5u)) ? top : 20u;
        }

        for (i = 0; i < 8u * 8u; i++) {
            memset(dst + ((TS_SEEK_H - 12u + i / 8u) * TS_SEEK_W + 6u + i % 8u) * 3u, 255, 3);
        }

        return;
    }

    src = ((t >= 30u && t < 45u) || t >= 128u) ? g_dark : g_wide;
    shift = (t >= 45u && t < 114u) ? 0u : t % TS_FRAMES;
    darkening = (t >= 50u && t < 114u) ? HDRG_MIN(t, 74u) - 49u : 0u;
    k = 1.0f;

    while (darkening > 0) {
        k *= 0.95f;
        darkening--;
    }

    for (y = 0; y < TS_SEEK_H; y++) {
        for (i = 0; i < row; i++) {
            dst[(size_t)y * row + i] =
                (uint8_t)((float)src[(size_t)y * wide + (size_t)shift * 3u + i] * k + 0.5f);
        }
    }
}

/* the format of the seek sequence's frames */
static void seek_format(hdrg_raw_image_t *format)
{
    memset(format, 0, sizeof(*format));
    format->width = TS_SEEK_W;
    format->height = TS_SEEK_H;
    format->bit_depth = 8;
    format->stride = (size_t)TS_SEEK_W * 3u;
    format->primaries = HDRG_PRIMARIES_BT709;
    format->transfer = HDRG_TRANSFER_SRGB;
}

/*
 * The seek sequence from frame start through a session of config, the first
 * preroll frames for its state alone: the rows and information of the count
 * frames received after them (fewer at the end), their number
 */
static uint32_t seek_run(hdrg_session_config_t config, const uint8_t *frames, uint32_t start,
                         uint32_t preroll, uint32_t count, uint8_t *rows,
                         hdrg_session_output_t *info)
{
    const size_t bytes = (size_t)TS_SEEK_W * TS_SEEK_H * 3u;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t sent, received;

    params_pq(&params, 1);
    seek_format(&format);
    config.start = start;
    config.preroll = preroll;
    config.rate_num = TS_SEEK_RATE;
    config.rate_den = 1;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));

    sent = start;
    received = 0;

    while (received < count) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = rows + (size_t)received * bytes;
        out.strides[0] = (size_t)TS_SEEK_W * 3u;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            memset(&in, 0, sizeof(in));
            in.planes[0] = frames + (size_t)sent * bytes;
            in.pts = (int64_t)sent;

            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TS_SEEK_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(out.pts == (int64_t)(start + preroll + received));
        info[received] = out;
        info[received].lights = keep_lights(&out);
        received++;
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    return received;
}

/* the preroll a session of config reports */
static uint32_t seek_preroll(hdrg_session_config_t config)
{
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t preroll;

    params_pq(&params, 1);
    seek_format(&format);
    config.rate_num = TS_SEEK_RATE;
    config.rate_den = 1;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 1));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       NULL, &config, &session));
    preroll = hdrg_session_preroll(session);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    return preroll;
}

/*
 * Seeking: a session started hdrg_session_preroll frames
 * before frame n (or at the first frame) renders frame n and the frames
 * after it byte for byte as the whole run does, through a pan, cuts, a
 * fade-out longer than its reach, a strobe the flash limiter works on and a
 * flash frame: WINDOWED with and without a window, TWO_PASS, STATIC,
 * WINDOWED with the PREFILTER stage (deblocking and noise reduction on
 * blocky, noisy frames) and WINDOWED under OFFLINE (the lights' long window
 * and the camera's motion). The whole run shows each of them at work
 */
static void test_seek(void)
{
    const uint32_t seeks[8] = {20, 72, 77, 95, 101, 116, 122, 133};
    const size_t bytes = (size_t)TS_SEEK_W * TS_SEEK_H * 3u;
    hdrg_session_config_t config;
    hdrg_session_output_t *whole, info[TS_SEEK_COUNT];
    hdrg_session_pass_t *passes;
    uint32_t t, s, n, preroll, start, got, limited;
    uint8_t *frames, *rows, *seek, round;
    size_t i;

    frames = (uint8_t *)malloc(bytes * TS_SEEK_FRAMES);
    rows = (uint8_t *)malloc(bytes * TS_SEEK_FRAMES);
    seek = (uint8_t *)malloc(bytes * TS_SEEK_COUNT);
    whole = (hdrg_session_output_t *)malloc(sizeof(*whole) * TS_SEEK_FRAMES);
    passes = (hdrg_session_pass_t *)malloc(sizeof(*passes) * TS_SEEK_FRAMES);
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL && seek != NULL && whole != NULL &&
                     passes != NULL);

    for (t = 0; t < TS_SEEK_FRAMES; t++) {
        seek_frame(t, frames + (size_t)t * bytes);
    }

    for (round = 0; round < 6u; round++) {
        if (!TS_FILTERED && (round == 0 || round == 2u || round == 4u || round == 5u)) {
            continue;
        }

        /* the deblocking and the NR: the build's */
        if (round == 4u && (HDRG_BUILTIN_DEBLOCK == 0 || HDRG_BUILTIN_DENOISE == 0)) {
            continue;
        }

        /*
         * PREFILTER: 8 x 8 blocks offset by up to +-3 codes and noise of
         * +-3 codes, both new every frame, for the deblocking and the NR
         */
        if (round == 4u) {
            for (i = 0; i < bytes * TS_SEEK_FRAMES; i++) {
                frames[i] = (uint8_t)HDRG_CLAMP(
                    (int32_t)frames[i] +
                        (int32_t)(hdrg_hash_uniform(TS_NOISE_KEY, (uint32_t)(i % bytes),
                                                    (uint32_t)(i / bytes), 0) *
                                  7.0f) +
                        (int32_t)(hdrg_hash_uniform(TS_NOISE_KEY,
                                                    (uint32_t)((i % bytes) / 3u % TS_SEEK_W / 8u),
                                                    (uint32_t)((i % bytes) / 3u / TS_SEEK_W / 8u),
                                                    1u + (uint32_t)(i / bytes)) *
                                  7.0f) -
                        6,
                    0, 255);
            }
        }

        g_grain = (round == 4u) ? HDRG_GRAIN_REDUCE : HDRG_GRAIN_PRESERVE;
        g_deblock = (round == 4u) ? 1.0f : 0.0f;
        config = rgb_config((round == 0 || round == 4u) ? 8u : 0u,
                            (round == 0 || round == 4u) ? 3u : 0u, 8);
        config.temporal = (round == 2u)   ? HDRG_TEMPORAL_TWO_PASS
                          : (round == 3u) ? HDRG_TEMPORAL_STATIC
                                          : HDRG_TEMPORAL_WINDOWED;

        /* OFFLINE: the statistics' future the past's by default */
        if (round == 5u) {
            config = rgb_config(8u, HDRG_SESSION_AUTO, 8);
            config.latency = HDRG_LATENCY_OFFLINE;
        }

        /* TWO_PASS: the first pass's statistics */
        if (round == 2u) {
            g_kept = 0;
            TEST_ASSERT_EQUAL_UINT32(TS_SEEK_FRAMES,
                                     seek_run(config, frames, 0, 0, TS_SEEK_FRAMES, rows, whole));

            for (t = 0; t < TS_SEEK_FRAMES; t++) {
                passes[t].lights = whole[t].lights;
                passes[t].light_count = whole[t].light_count;
                passes[t].anchor_luma = whole[t].anchor_luma;
                passes[t].key_luma = whole[t].key_luma;
                passes[t].apl_fit = whole[t].apl_fit;
                passes[t].scene_cut = whole[t].scene_cut;
            }

            config.passes = passes;
            config.pass_count = TS_SEEK_FRAMES;
        }

        TEST_ASSERT_EQUAL_UINT32(TS_SEEK_FRAMES,
                                 seek_run(config, frames, 0, 0, TS_SEEK_FRAMES, rows, whole));
        preroll = seek_preroll(config);
        limited = 0;

        for (t = 0; t < TS_SEEK_FRAMES; t++) {
            limited += whole[t].flash_limited ? 1u : 0u;
        }

        if (round == 0) {
            /* max(5, 20 + 2 * 8 + 4) + 10 + 1 */
            TEST_ASSERT_EQUAL_UINT32(51, preroll);

            for (t = 0; t < TS_SEEK_FRAMES; t++) {
                TEST_ASSERT_TRUE(whole[t].fading == (t >= 52u && t < 75u));
                TEST_ASSERT_TRUE(!whole[t].scene_cut || t == 0 || t == 30u || t == 45u ||
                                 t == 80u || t == 114u || t == 120u || t == 128u);
            }

            TEST_ASSERT_TRUE(whole[30].scene_cut && whole[45].scene_cut && whole[114].scene_cut &&
                             whole[128].scene_cut);
            TEST_ASSERT_TRUE(whole[60].anchor_used == whole[52].anchor_used &&
                             whole[60].apl_scale == whole[52].apl_scale);
            TEST_ASSERT_TRUE(whole[72].anchor_used != whole[52].anchor_used);
            TEST_ASSERT_TRUE(limited > 0);
        }

        /* the deblocking and the NR are at work: the run without either differs */
        if (round == 4u) {
            g_deblock = 0.0f;
            TEST_ASSERT_EQUAL_UINT32(TS_SEEK_COUNT,
                                     seek_run(config, frames, 0, 0, TS_SEEK_COUNT, seek, info));
            TEST_ASSERT_TRUE(memcmp(rows, seek, bytes * TS_SEEK_COUNT) != 0);
            g_grain = HDRG_GRAIN_PRESERVE;
            g_deblock = 1.0f;
            TEST_ASSERT_EQUAL_UINT32(TS_SEEK_COUNT,
                                     seek_run(config, frames, 0, 0, TS_SEEK_COUNT, seek, info));
            TEST_ASSERT_TRUE(memcmp(rows, seek, bytes * TS_SEEK_COUNT) != 0);
            g_grain = HDRG_GRAIN_REDUCE;
        }

        if (round == 3u) {
            TEST_ASSERT_EQUAL_UINT32(0, preroll);
            TEST_ASSERT_EQUAL_UINT32(0, limited);
        } else {
            TEST_ASSERT_TRUE(limited > 0);
        }

        for (s = 0; s < 8u; s++) {
            n = seeks[s];
            start = (n > preroll) ? n - preroll : 0u;
            got = seek_run(config, frames, start, n - start, TS_SEEK_COUNT, seek, info);
            TEST_ASSERT_EQUAL_UINT32(HDRG_MIN(TS_SEEK_COUNT, TS_SEEK_FRAMES - n), got);

            for (t = 0; t < got; t++) {
                TEST_ASSERT_EQUAL_MEMORY(rows + (size_t)(n + t) * bytes, seek + (size_t)t * bytes,
                                         bytes);
                TEST_ASSERT_TRUE(info[t].apl_scale == whole[n + t].apl_scale &&
                                 info[t].anchor_used == whole[n + t].anchor_used &&
                                 info[t].apl_nits == whole[n + t].apl_nits &&
                                 info[t].max_nits == whole[n + t].max_nits &&
                                 info[t].scene_cut == whole[n + t].scene_cut &&
                                 info[t].flash_limited == whole[n + t].flash_limited &&
                                 info[t].fading == whole[n + t].fading);
            }
        }
    }

    g_grain = HDRG_GRAIN_PRESERVE;
    g_deblock = 0.0f;
    free(frames);
    free(rows);
    free(seek);
    free(whole);
    free(passes);
}

/* the seek sequence's config, its rate (the reference run's: seek_run) */
static hdrg_session_config_t ts_drive_config(void)
{
    hdrg_session_config_t config;

    config = rgb_config(TS_PAST, TS_FUTURE, 8);
    config.rate_num = TS_SEEK_RATE;
    config.rate_den = 1;

    return config;
}

/* the next frame, or the end, into the drive's session */
static hdrg_status_t ts_drive_send(ts_drive_t *d)
{
    hdrg_session_frame_t in;
    hdrg_status_t status;

    if (d->sent == d->count) {
        status = hdrg_session_send_frame(d->session, NULL);
        d->sent += (status == HDRG_OK) ? 1u : 0u;

        return status;
    }

    memset(&in, 0, sizeof(in));
    in.planes[0] = d->frames + (size_t)d->sent * TS_SEEK_BYTES;
    in.pts = (int64_t)d->sent;

    status = hdrg_session_send_frame(d->session, &in);
    d->sent += (status == HDRG_OK) ? 1u : 0u;

    return status;
}

/*
 * The drive's next output (false at the end or on a failure): a lazy drive
 * sends as far ahead as the session takes before each receive (the deepest
 * pipeline), the other sends a frame only when a receive asks for one
 */
static bool ts_drive_step(ts_drive_t *d)
{
    hdrg_session_output_t out;
    hdrg_status_t status;

    while (d->lazy && d->sent <= d->count) {
        status = ts_drive_send(d);
        if (status == HDRG_ERR_AGAIN) {
            break;
        }

        if (status != HDRG_OK) {
            d->failed = true;

            return false;
        }
    }

    for (;;) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = d->rows + (size_t)d->received * TS_SEEK_BYTES;
        out.strides[0] = (size_t)TS_SEEK_W * 3u;

        status = hdrg_session_receive_frame(d->session, &out);
        if (status == HDRG_OK) {
            d->received++;

            return true;
        }

        if (status == HDRG_EOF) {
            return false;
        }

        if (status != HDRG_ERR_AGAIN || d->sent > d->count || ts_drive_send(d) != HDRG_OK) {
            d->failed = true;

            return false;
        }
    }
}

/* a whole drive in a context of its own (a thread's: no test assertions) */
static void ts_drive_worker(void *arg)
{
    const hdrg_session_config_t config = ts_drive_config();
    ts_drive_t *d = (ts_drive_t *)arg;
    hdrg_raw_image_t format;
    hdrg_context_t *ctx;

    seek_format(&format);

    if (hdrg_test_context(&ctx, NULL, d->threads) != HDRG_OK) {
        d->failed = true;

        return;
    }

    if (hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), d->params, NULL, &config,
                            &d->session) == HDRG_OK) {
        while (ts_drive_step(d)) {
            continue;
        }

        hdrg_session_destroy(d->session);
    } else {
        d->failed = true;
    }

    hdrg_context_destroy(ctx);
}

/*
 * The seek sequence's first TS_DRIVE_FRAMES frames, then the end, and the
 * rows of their single run (a context of one thread, a frame sent only when
 * a receive asks for one)
 */
static void ts_drive_reference(const hdrg_params_t *params, uint8_t *frames, uint8_t *reference)
{
    ts_drive_t d;
    uint32_t t;

    for (t = 0; t < TS_DRIVE_FRAMES; t++) {
        seek_frame(t, frames + (size_t)t * TS_SEEK_BYTES);
    }

    memset(&d, 0, sizeof(d));
    d.params = params;
    d.frames = frames;
    d.rows = reference;
    d.count = TS_DRIVE_FRAMES;
    d.threads = 1;

    ts_drive_worker(&d);
    TEST_ASSERT_FALSE(d.failed);
    TEST_ASSERT_EQUAL_UINT32(TS_DRIVE_FRAMES, d.received);
}

/*
 * Determinism across sessions: sessions in contexts of their own
 * running at once on threads (each context with a pool of its own) equal
 * the single run byte for byte
 */
static void test_sessions_concurrent(void)
{
    const size_t bytes = TS_SEEK_BYTES * TS_DRIVE_FRAMES;
    ts_drive_t drives[3];
    hdrg_params_t params;
    hdrg_thread_t threads[3];
    uint8_t *frames, *reference, *rows, i;

    HDRG_TEST_REQUIRE_THREADS();

    frames = (uint8_t *)malloc(bytes);
    reference = (uint8_t *)malloc(bytes);
    rows = (uint8_t *)malloc(bytes * 3u);
    TEST_ASSERT_TRUE(frames != NULL && reference != NULL && rows != NULL);
    params_pq(&params, 2);
    ts_drive_reference(&params, frames, reference);
    memset(drives, 0, sizeof(drives));

    for (i = 0; i < 3u; i++) {
        drives[i].params = &params;
        drives[i].frames = frames;
        drives[i].rows = rows + (size_t)i * bytes;
        drives[i].count = TS_DRIVE_FRAMES;
        drives[i].threads = 2;
        drives[i].lazy = i == 1u;
        TEST_ASSERT_TRUE(hdrg_thread_create(&threads[i], ts_drive_worker, &drives[i]));
    }

    for (i = 0; i < 3u; i++) {
        hdrg_thread_join(&threads[i]);
        TEST_ASSERT_FALSE(drives[i].failed);
        TEST_ASSERT_EQUAL_UINT32(TS_DRIVE_FRAMES, drives[i].received);
        TEST_ASSERT_EQUAL_MEMORY(reference, drives[i].rows, bytes);
    }

    free(frames);
    free(reference);
    free(rows);
}

/*
 * Two sessions taking turns on one context, one sending as far ahead as it
 * can (the deepest pipeline) and one sending only when asked, equal the
 * single run byte for byte
 */
static void test_sessions_interleaved(void)
{
    const hdrg_session_config_t config = ts_drive_config();
    const size_t bytes = TS_SEEK_BYTES * TS_DRIVE_FRAMES;
    ts_drive_t drives[2];
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_context_t *ctx;
    uint8_t *frames, *reference, *rows, i;
    bool more;

    frames = (uint8_t *)malloc(bytes);
    reference = (uint8_t *)malloc(bytes);
    rows = (uint8_t *)malloc(bytes * 2u);
    TEST_ASSERT_TRUE(frames != NULL && reference != NULL && rows != NULL);
    params_pq(&params, 2);
    ts_drive_reference(&params, frames, reference);
    seek_format(&format);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));
    memset(drives, 0, sizeof(drives));

    for (i = 0; i < 2u; i++) {
        drives[i].params = &params;
        drives[i].frames = frames;
        drives[i].rows = rows + (size_t)i * bytes;
        drives[i].count = TS_DRIVE_FRAMES;
        drives[i].lazy = i == 0;
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                           NULL, &config, &drives[i].session));
    }

    do {
        more = ts_drive_step(&drives[0]);
        more = ts_drive_step(&drives[1]) || more;
    } while (more);

    for (i = 0; i < 2u; i++) {
        TEST_ASSERT_FALSE(drives[i].failed);
        TEST_ASSERT_EQUAL_UINT32(TS_DRIVE_FRAMES, drives[i].received);
        TEST_ASSERT_EQUAL_MEMORY(reference, drives[i].rows, bytes);
        hdrg_session_destroy(drives[i].session);
    }

    hdrg_context_destroy(ctx);
    free(frames);
    free(reference);
    free(rows);
}

/*
 * hdrg_session_peak: the display's peak, else 1000 cd/m^2, within its
 * headroom; no frame's rendition peak exceeds it
 */
static void test_peak_bound(void)
{
    const hdrg_session_config_t config = rgb_config(TS_PAST, 0, 8);
    hdrg_session_output_t info[TS_FRAMES];
    hdrg_display_caps_t caps;
    hdrg_params_t params;
    hdrg_raw_image_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t t;
    uint8_t *rows;

    params_pq(&params, 1);
    format_of(&format);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, g_threads));

    hdrg_display_caps_init(&caps);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       &caps, &config, &session));
    TEST_ASSERT_TRUE(hdrg_session_peak(session) == 1000.0f);
    hdrg_session_destroy(session);

    caps.peak_nits = 600.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       &caps, &config, &session));
    TEST_ASSERT_TRUE(hdrg_session_peak(session) == 600.0f);
    hdrg_session_destroy(session);

    caps.peak_nits = 0.0f;
    caps.headroom_stops = 1.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&format), &params,
                                                       &caps, &config, &session));
    TEST_ASSERT_TRUE(hdrg_session_peak(session) == 406.0f);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    rows = (uint8_t *)malloc((size_t)TS_W * 3u * TS_H * TS_FRAMES);
    TEST_ASSERT_NOT_NULL(rows);
    session_rows(&config, 1, TS_SCENE, (size_t)TS_W * 3u, rows, info);

    for (t = 0; t < TS_FRAMES; t++) {
        TEST_ASSERT_TRUE(info[t].peak_nits > 0.0f && info[t].peak_nits <= 1000.0f);
    }

    free(rows);
}

void setUp(void)
{
    g_kept = 0;
    g_grain = HDRG_GRAIN_PRESERVE;
    g_deblock = 0.0f;
    g_deband = 0.0f;
}

void tearDown(void)
{
}

int main(void)
{
    int failures;
    size_t i, bytes;

    g_wide = hdrg_test_image(TS_W + TS_FRAMES, TS_H);
    g_dark = hdrg_test_image(TS_W + TS_FRAMES, TS_H);
    g_patch = (uint8_t *)malloc((size_t)(TS_W + TS_FRAMES) * TS_H * 3u);
    if (g_wide == NULL || g_dark == NULL || g_patch == NULL) {
        return 1;
    }

    /* the second scene: the same picture two stops darker; the patch: 18 x 18 white */
    bytes = (size_t)(TS_W + TS_FRAMES) * TS_H * 3u;
    for (i = 0; i < bytes; i++) {
        g_dark[i] = (uint8_t)(g_dark[i] / 4u);
        g_patch[i] = g_dark[i];
    }

    for (i = 0; i < 18u * 18u; i++) {
        memset(g_patch + ((40u + i / 18u) * (TS_W + TS_FRAMES) + 60u + i % 18u) * 3u, 255, 3);
    }

    /* the noise: a counter hash of the sample, uniform in [-3, 3] */
    g_noisy = (uint8_t *)malloc(bytes);
    if (g_noisy == NULL) {
        return 1;
    }

    for (i = 0; i < bytes; i++) {
        g_noisy[i] = (uint8_t)HDRG_CLAMP(
            (int32_t)g_wide[i] +
                (int32_t)(hdrg_hash_uniform(TS_NOISE_KEY, (uint32_t)i, 0, 0) * 7.0f) - 3,
            0, 255);
    }

    UNITY_BEGIN();
    RUN_TEST(test_zero_window_matches_still);
    RUN_TEST(test_scaled_matches_still);
    RUN_TEST(test_threads_and_stride_identical);
    RUN_TEST(test_scene_cut);
    RUN_TEST(test_protocol);
    RUN_TEST(test_flicker);
    RUN_TEST(test_depth16_and_light);
    RUN_TEST(test_bt2020_container);
    RUN_TEST(test_yuv_input);
    RUN_TEST(test_yuv_output);
    RUN_TEST(test_proxy);
    RUN_TEST(test_defaults);
    RUN_TEST(test_steady_allocations);
    RUN_TEST(test_steady_band_filters);
    RUN_TEST(test_recursive);
    RUN_TEST(test_two_pass);
    RUN_TEST(test_filters_off);
    RUN_TEST(test_static);
    RUN_TEST(test_flash);
    RUN_TEST(test_letterbox);
    RUN_TEST(test_fade);
    RUN_TEST(test_fade_to_black);
    RUN_TEST(test_prefilter);
    RUN_TEST(test_seek);
    RUN_TEST(test_prefilter_idle);
    RUN_TEST(test_peak_bound);
    RUN_TEST(test_sessions_concurrent);
    RUN_TEST(test_sessions_interleaved);
    failures = UNITY_END();

    free(g_wide);
    free(g_dark);
    free(g_patch);
    free(g_noisy);

    return failures;
}
