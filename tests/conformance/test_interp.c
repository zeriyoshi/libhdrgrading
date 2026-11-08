/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <math.h>
#include <unity.h>

#include "../test_util.h"
#include "hdrgrading/foundation/clock.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/thread.h"
#include "core/motion.h"
#include "hdrgrading/filter.h"
#include "hdrgrading/video.h"
#include "host/stage.h"
#include "priv.h"

/*
 * Frame interpolation (the INTERP stage): the built-in filters
 * on their own (the blend's integers, the motion-compensated frame of a
 * moving texture near the truth and far nearer than the blend, the same
 * whatever its threads) and a video session at another output rate: the
 * frames' count, places and pts; its input frames the same bytes as a
 * session at the input's rate; the repeat filter's frames the nearer
 * frame's output; the nearer frame across a cut; a frame with no output
 * still in the statistics; a seek with the preroll the same bytes; a
 * filter of the app's told the phases; a filter's window around the pair
 * (the frames, its shot, every phase of the interval, a seek); SCALE and
 * the output rate together (the frames between the frames scaled); the
 * rejected configurations. The
 * motion (HDRG_SD_MOTION): its layout checked and the gain cap's rule;
 * hdrg.interp.mc's output motion (the texture's vectors, sure; its GPU
 * versions' the same bytes), the app's frames' motion to the INTERP jobs,
 * the cap of a frame between where the motion is unsure (a lamp that
 * jumps: dimmer; a lamp followed: as bright; a sure motion: the nearer
 * frame's bytes) and a seek with it the same bytes.
 */

#define TI_W        96u
#define TI_H        64u
#define TI_FRAMES   10u
#define TI_MAX_OUT  256u
#define TI_VX       6.0 /* the texture's motion, pixels a frame */
#define TI_VY       2.0
#define TI_MAX_JOBS 64u
#define TI_SEEK     124u /* the seek test's frames: the default preroll (112 at 30 fps) and more */
#define TI_MOTION   65536u /* a stage's output motion kept, bytes at most */
#define TI_LAMP_X0  24.0   /* the lamp's place: (24, 32), jumping to (72, 32) ... */
#define TI_LAMP_X1  72.0
#define TI_LAMP_Y   32.0
#define TI_LAMP_V   2.0 /* ... or moving 2 pixels a frame */

typedef struct {
    uint8_t *frames; /* each output frame's 16-bit PQ R'G'B' */
    int64_t pts[TI_MAX_OUT];
    int64_t src_pts[TI_MAX_OUT][2];
    uint32_t phase[TI_MAX_OUT];
    bool synthetic[TI_MAX_OUT];
    float max_nits[TI_MAX_OUT];
    uint32_t count;
} ti_run_t;

/* the asynchronous filter's job, ended from a thread of its own */
typedef struct {
    hdrg_thread_t thread;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    bool running;
} ti_flight_t;

/* the window filter's jobs: the frame, its window and shot, the phases, the frames as sent */
typedef struct {
    uint32_t index, past, future, shot_past, shot_future, phase, phase_count, phases[4];
    bool frames_ok;
} ti_window_job_t;

static uint32_t g_phases[TI_MAX_JOBS]; /* the app's filter's jobs' phases */
static uint32_t g_jobs;
static uint8_t g_motion[TI_MOTION]; /* ti_between_size: the stage's output motion ... */
static size_t g_motion_size;        /* ... its bytes (0: none) */
static uint8_t g_content;      /* the session's frames: 0 the texture, 1 a lamp jumping, 2 a lamp
                                  moving */
static bool g_send_motion;     /* the session's frames sent with motion: mv_x the frame's index */
static uint32_t g_mctf_fields; /* ti_seen: the fields of hdrg.mctf seen */
static uint32_t g_mctf_fields_now; /* ... in the motion being read */
static uint8_t g_app_mode; /* the app's filter: 0 frame a; 1 frame b and a motion of g_app_conf */
static uint8_t g_app_conf;
static int32_t g_seen_mvs[TI_MAX_JOBS]; /* the app's filter's jobs: the input motion's first mv_x
                                           (-1: none) */
static hdrg_status_t g_refusals[3];     /* ... job_set_side_data's of a wrong type, version, size */
static bool g_prefilter;                /* the session with the app's PREFILTER filter (a window) */
static int32_t g_pre_seen[TI_MAX_JOBS]; /* ... its frames' motion's first mv_x, in order */
static uint32_t g_pre_count;
static ti_window_job_t g_window_jobs[TI_MAX_JOBS]; /* the window filter's jobs ... */
static uint32_t g_window_count;                    /* ... of them */
static uint32_t g_cut = UINT32_MAX;                /* ti_session's cut (the frames as sent) */
static bool g_static;                              /* ti_session: STATIC, no flash limiter */
static bool g_rgba;          /* ti_session: the frames sent as RGBA (alpha 255) */
static int64_t g_pts0;       /* ti_session: frame k's pts 1000 k + this */
static uint32_t g_scale = 1; /* ti_session: the output's size over the input's */
static bool g_gpu;           /* ti_session: the context's GPU FILTERS */
static uint32_t g_app_w;     /* the app's filters: the width configured */
static uint8_t g_expect[TI_W * TI_H * 3u];
static ti_flight_t g_flights[8];   /* the asynchronous filter's jobs ... */
static hdrg_mutex_t g_flight_lock; /* ... its counts' lock ... */
static uint32_t g_flight_submits, g_flight_done, g_flight_peak; /* ... jobs submitted, ended, the
                                                                    most in flight at once */

/* the texture at (x, y) at time t (frames): smooth bands moving by (TI_VX, TI_VY) a frame */
static double ti_texture(double x, double y, double t)
{
    const double u = x - TI_VX * t, v = y - TI_VY * t;

    return 0.5 + 0.18 * sin(u * 0.31) * cos(v * 0.23) + 0.12 * sin((u + v) * 0.11) +
           0.08 * cos(u * 0.07 - v * 0.19);
}

/* the 16-bit R'G'B' of the texture at time t */
static void ti_rgb16(uint16_t *rgb, double t)
{
    uint32_t x, y;
    double v;

    for (y = 0; y < TI_H; y++) {
        for (x = 0; x < TI_W; x++) {
            v = ti_texture((double)x, (double)y, t);
            rgb[((size_t)y * TI_W + x) * 3u] = (uint16_t)(v * 65535.0 + 0.5);
            rgb[((size_t)y * TI_W + x) * 3u + 1u] = (uint16_t)(v * 0.8 * 65535.0 + 0.5);
            rgb[((size_t)y * TI_W + x) * 3u + 2u] = (uint16_t)((1.0 - v * 0.6) * 65535.0 + 0.5);
        }
    }
}

/*
 * the session's frame k: the texture panned, or (with a cut at frame cut) a
 * bright scene after; g_content 1 and 2: a lamp (a white core and its glow)
 * on a dark wall, jumping between two places or moving
 */
static void ti_frame8(uint8_t *rgb, uint32_t k, uint32_t cut)
{
    uint32_t x, y;
    uint8_t c;
    double v, lx, d;

    lx = (g_content == 1u) ? ((k % 2u == 0u) ? TI_LAMP_X0 : TI_LAMP_X1)
                           : TI_LAMP_X0 + TI_LAMP_V * (double)k;

    for (y = 0; y < TI_H && g_content != 0u; y++) {
        for (x = 0; x < TI_W; x++) {
            d = sqrt(((double)x - lx) * ((double)x - lx) +
                     ((double)y - TI_LAMP_Y) * ((double)y - TI_LAMP_Y));
            v = (d < 2.0) ? 1.0 : 0.04 + 0.7 * exp(-(d - 2.0) * (d - 2.0) / 18.0);

            for (c = 0; c < 3u; c++) {
                rgb[((size_t)y * TI_W + x) * 3u + c] = (uint8_t)(v * 255.0 + 0.5);
            }
        }
    }

    for (y = 0; y < TI_H && g_content == 0u; y++) {
        for (x = 0; x < TI_W; x++) {
            v = ti_texture((double)x, (double)y, (double)k);

            for (c = 0; c < 3u; c++) {
                rgb[((size_t)y * TI_W + x) * 3u + c] =
                    (k >= cut) ? (uint8_t)(200u + (x * 3u + y + c * 7u) % 50u)
                               : (uint8_t)(v * (0.4 + 0.25 * c) * 255.0 + 0.5);
            }
        }
    }
}

/*
 * the frame of width x height at phase between a and b from the INTERP
 * filter id (options: key=value or NULL; threads=N: on a context of N
 * threads), its motion in g_motion;
 * HDRG_ERR_NOT_FOUND without the class, HDRG_ERR_UNSUPPORTED without its
 * device
 */
static hdrg_status_t ti_between_size(const char *id, const char *option, uint32_t width,
                                     uint32_t height, const uint16_t *a, const uint16_t *b,
                                     uint32_t phase, uint16_t *out)
{
    const uint16_t *rgb;
    const uint8_t *motion;
    hdrg_context_t *ctx;
    hdrg_options_t *options;
    hdrg_filter_entry_t entry;
    hdrg_source_t sa, sb;
    hdrg_stage_t stage;
    hdrg_stage_between_t between;
    hdrg_stage_frame_t frame;
    hdrg_exec_t exec;
    hdrg_status_t status;
    uint32_t y, threads;
    size_t stride, motion_size;
    char key[32];

    threads = (option != NULL && strncmp(option, "threads=", 8) == 0)
                  ? (uint32_t)strtoul(option + 8, NULL, 10)
                  : 0u;
    option = (threads > 0) ? NULL : option;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));

    if (option != NULL) {
        snprintf(key, sizeof(key), "%s", option);
        key[strcspn(key, "=")] = '\0';
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, key, strchr(option, '=') + 1));
    }

    memset(&entry, 0, sizeof(entry));
    entry.cls = hdrg_host_find_class(&ctx->host, id, strlen(id));

    if (entry.cls == NULL) {
        hdrg_options_destroy(options);
        hdrg_context_destroy(ctx);

        return HDRG_ERR_NOT_FOUND;
    }

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_host_resolve_options(&ctx->host, entry.cls, options, &entry.options));
    entry.stage = HDRG_STAGE_INTERP;

    memset(&sa, 0, sizeof(sa));
    sa.width = width;
    sa.height = height;
    sa.stride = (size_t)width * 6u;
    sa.comps = 3;
    sa.wide = true;
    sa.entries = 65536;
    sb = sa;
    sa.pixels = (const uint8_t *)a;
    sb.pixels = (const uint8_t *)b;
    memset(&between, 0, sizeof(between));
    between.frames[0] = &sa;
    between.frames[1] = &sb;
    between.future = 1;
    between.shot_future = 1;
    between.phase = phase;
    memset(&frame, 0, sizeof(frame));
    frame.bits = 16;
    exec.pool = ctx->pool;
    exec.max_workers = UINT32_MAX;

    memset(&stage, 0, sizeof(stage));
    status = hdrg_stage_create(&ctx->alloc, &entry, 256, &ctx->host, &stage);

    if (status == HDRG_OK) {
        status = hdrg_stage_interp(&stage, &exec, &between, &frame, &ctx->host, &rgb, &stride,
                                   &motion, &motion_size);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        g_motion_size = motion_size;
        TEST_ASSERT_TRUE(g_motion_size <= TI_MOTION);

        if (motion != NULL) {
            memcpy(g_motion, motion, motion_size);
        }

        for (y = 0; y < height; y++) {
            memcpy(out + (size_t)y * width * 3u, (const uint8_t *)rgb + (size_t)y * stride,
                   (size_t)width * 6u);
        }

        hdrg_stage_close(&stage);
    }

    hdrg_options_destroy(entry.options);
    hdrg_options_destroy(options);
    hdrg_context_destroy(ctx);

    return status;
}

/* the frame at phase between a and b from the INTERP filter id (options: key=value or NULL) */
static void ti_between(const char *id, const char *option, const uint16_t *a, const uint16_t *b,
                       uint32_t phase, uint16_t *out)
{
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_between_size(id, option, TI_W, TI_H, a, b, phase, out));
}

#if HDRG_BUILTIN_INTERP_MC
/* PSNR (16-bit) of got against want over the pixels 16 or more from the edges */
static double ti_psnr(const uint16_t *got, const uint16_t *want)
{
    uint32_t x, y;
    uint8_t c;
    size_t n;
    double sum, d;

    sum = 0.0;
    n = 0;

    for (y = 16; y < TI_H - 16u; y++) {
        for (x = 16; x < TI_W - 16u; x++) {
            for (c = 0; c < 3u; c++) {
                d = (double)got[((size_t)y * TI_W + x) * 3u + c] -
                    (double)want[((size_t)y * TI_W + x) * 3u + c];
                sum += d * d;
                n++;
            }
        }
    }

    return 10.0 * log10(65535.0 * 65535.0 / HDRG_TEST_MAX(sum / (double)n, 1e-9));
}

#endif

static hdrg_status_t HDRG_PLUGIN_CALL ti_app_create(hdrg_filter_t *filter,
                                                    const hdrg_options_t *options, void **inst,
                                                    const hdrg_plugin_api_t *api)
{
    (void)filter;
    (void)options;
    (void)api;
    *inst = &g_jobs;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL ti_app_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;
}

/*
 * the first mv_x of the input frame's motion of the app (HDRG_MOTION_HOST; -1: none); the
 * fields hdrg.mctf adds after the app's (the INTERP jobs') counted into g_mctf_fields
 */
static int32_t ti_seen(const hdrg_negotiation_t *nego, const hdrg_plugin_api_t *api)
{
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field;
    hdrg_sd_motion_block_t block;
    hdrg_status_t status;
    uint32_t version, k;
    static uint8_t buf[65536];
    int32_t seen;
    size_t needed, at;

    status = api->nego_side_data(nego, HDRG_SD_MOTION, &version, buf, sizeof(buf), &needed);
    if (status != HDRG_OK) {
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, status);

        return -1;
    }

    TEST_ASSERT_EQUAL_UINT32(HDRG_SD_MOTION_VERSION, version);
    TEST_ASSERT_TRUE(needed <= sizeof(buf) && hdrg_motion_check(buf, needed));
    memcpy(&head, buf, sizeof(head));
    seen = -1;
    at = sizeof(head);

    for (k = 0; k < head.fields; k++) {
        memcpy(&field, buf + at, sizeof(field));
        memcpy(&block, buf + at + sizeof(field), sizeof(block));

        if (field.source == HDRG_MOTION_HOST && seen < 0) {
            seen = block.mv_x;
        }

        /* hdrg.mctf's after the app's */
        if (field.source == HDRG_MOTION_INTERNAL) {
            TEST_ASSERT_TRUE(field.ref_offset != 0 && field.phase == 0u);
            g_mctf_fields++;
        } else {
            TEST_ASSERT_EQUAL_UINT32(0, g_mctf_fields_now);
        }

        g_mctf_fields_now += (field.source == HDRG_MOTION_INTERNAL) ? 1u : 0u;
        at += sizeof(field) + (size_t)field.grid_w * field.grid_h * sizeof(block);
    }

    g_mctf_fields_now = 0;

    return seen;
}

/* the app's filters: the input frame's motion seen (the INTERP job's, the PREFILTER frame's) */
static hdrg_status_t HDRG_PLUGIN_CALL ti_app_configure(void *inst, hdrg_negotiation_t *nego,
                                                       const hdrg_plugin_api_t *api)
{
    uint32_t width, height, format;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->nego_input(nego, &width, &height, &format));
    g_app_w = width;

    if (inst == (void *)&g_jobs && g_jobs < TI_MAX_JOBS) {
        g_seen_mvs[g_jobs] = ti_seen(nego, api);
    }

    if (inst == (void *)&g_pre_count && g_pre_count < TI_MAX_JOBS) {
        g_pre_seen[g_pre_count++] = ti_seen(nego, api);
    }

    return api->nego_set_output(nego, width, height, HDRG_FORMAT_RGB16);
}

/* the app's PREFILTER filter: a window of the frame after it */
static hdrg_status_t HDRG_PLUGIN_CALL ti_pre_create(hdrg_filter_t *filter,
                                                    const hdrg_options_t *options, void **inst,
                                                    const hdrg_plugin_api_t *api)
{
    (void)options;
    *inst = &g_pre_count;

    return api->filter_set_window(filter, 0, 1);
}

/* ... the frame given back */
static hdrg_status_t HDRG_PLUGIN_CALL ti_pre_process(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const uint8_t *in;
    uint32_t begin, end, y;
    uint8_t *out;
    size_t in_stride, out_stride;

    (void)inst;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_input(job, 0, &in, &in_stride, &begin, &end));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_output(job, 0, &out, &out_stride));

    for (y = begin; y < end; y++) {
        memcpy(out + (size_t)y * out_stride, in + (size_t)y * in_stride, (size_t)TI_W * 6u);
    }

    return HDRG_OK;
}

/*
 * one field of zero vectors of blocks of 16 x 16 pixels over the frame at
 * phase, each of confidence conf and the first's mv_x first, into blob
 * (*size bytes)
 */
static void ti_motion_blob(uint8_t conf, int16_t first, uint32_t phase, uint8_t *blob, size_t *size)
{
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field;
    hdrg_sd_motion_block_t block;
    uint32_t i;

    memset(&head, 0, sizeof(head));
    memset(&field, 0, sizeof(field));
    memset(&block, 0, sizeof(block));
    head.fields = 1;
    field.source = HDRG_MOTION_HOST;
    field.ref_offset = 1;
    field.block_w = 16;
    field.block_h = 16;
    field.grid_w = (TI_W + 15u) / 16u;
    field.grid_h = (TI_H + 15u) / 16u;
    field.phase = phase;
    field.signal = HDRG_MOTION_SIGNAL_NONE;
    block.confidence = conf;
    memcpy(blob, &head, sizeof(head));
    memcpy(blob + sizeof(head), &field, sizeof(field));
    *size = sizeof(head) + sizeof(field);

    for (i = 0; i < field.grid_w * field.grid_h; i++) {
        block.mv_x = (i == 0u) ? first : 0;
        memcpy(blob + *size, &block, sizeof(block));
        *size += sizeof(block);
    }
}

/*
 * the app's filter: the phase recorded, the first frame given back (both
 * frames read); g_app_mode 1: the second, its motion g_app_conf sure (after
 * the refusals of a wrong type, version and size)
 */
static hdrg_status_t HDRG_PLUGIN_CALL ti_app_process(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const uint8_t *a, *b;
    uint32_t begin, end, y, phase;
    uint8_t *out, blob[512];
    size_t a_stride, b_stride, out_stride, size;

    (void)inst;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_input(job, 0, &a, &a_stride, &begin, &end));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_input_frame(job, 1, 0, &b, &b_stride, &begin, &end));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND,
                          api->job_input_frame(job, 2, 0, &b, &b_stride, &begin, &end));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_output(job, 0, &out, &out_stride));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_phase(job, &phase));

    if (g_jobs < TI_MAX_JOBS) {
        g_phases[g_jobs] = phase;
    }

    g_jobs++;

    for (y = begin; y < end; y++) {
        memcpy(out + (size_t)y * out_stride,
               (g_app_mode == 1u) ? b + (size_t)y * b_stride : a + (size_t)y * a_stride,
               (size_t)g_app_w * 6u);
    }

    if (g_app_mode != 1u) {
        return HDRG_OK;
    }

    ti_motion_blob(g_app_conf, 0, phase, blob, &size);
    g_refusals[0] = api->job_set_side_data(job, HDRG_SD_NOISE, HDRG_SD_NOISE_VERSION, blob, size);
    g_refusals[1] = api->job_set_side_data(job, HDRG_SD_MOTION, 2, blob, size);
    g_refusals[2] =
        api->job_set_side_data(job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION, blob, size - 1u);

    return api->job_set_side_data(job, HDRG_SD_MOTION, HDRG_SD_MOTION_VERSION, blob, size);
}

/* the window filter: 2 frames before t and 3 after it (past the limits refused) */
static hdrg_status_t HDRG_PLUGIN_CALL ti_window_create(hdrg_filter_t *filter,
                                                       const hdrg_options_t *options, void **inst,
                                                       const hdrg_plugin_api_t *api)
{
    (void)options;
    *inst = &g_window_count;

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, api->filter_set_window(filter, 5, 1));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, api->filter_set_window(filter, 0, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, api->filter_set_window(filter, 0, 6));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, api->filter_set_clip(filter, 4, 0));

    return api->filter_set_window(filter, 2, 3);
}

/*
 * ... each job recorded (its frames checked against those sent: row 0),
 * its output the rounded mean of its shot's frames
 */
static hdrg_status_t HDRG_PLUGIN_CALL ti_window_process(void *inst, hdrg_job_t *job,
                                                        const hdrg_plugin_api_t *api)
{
    const uint8_t *in[9];
    ti_window_job_t *rec, scratch;
    uint32_t begin, end, y, x, sum;
    uint16_t sample;
    uint8_t *out;
    int32_t k;
    size_t strides[9], out_stride;

    (void)inst;

    rec = (g_window_count < TI_MAX_JOBS) ? &g_window_jobs[g_window_count] : &scratch;
    g_window_count++;
    memset(rec, 0, sizeof(*rec));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_window(job, &rec->index, &rec->past, &rec->future));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_shot(job, &rec->shot_past, &rec->shot_future));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_phase(job, &rec->phase));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_phases(job, rec->phases, 4, &rec->phase_count));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, api->job_output(job, 0, &out, &out_stride));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_NOT_FOUND,
        api->job_input_frame(job, -(int32_t)rec->past - 1, 0, &in[0], &strides[0], &begin, &end));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_NOT_FOUND,
        api->job_input_frame(job, (int32_t)rec->future + 1, 0, &in[0], &strides[0], &begin, &end));
    rec->frames_ok = rec->past <= 2u && rec->future >= 1u && rec->future <= 3u;

    for (k = -(int32_t)HDRG_TEST_MIN(rec->past, 2u); k <= (int32_t)HDRG_TEST_MIN(rec->future, 3u);
         k++) {
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, api->job_input_frame(job, k, 0, &in[k + 2], &strides[k + 2], &begin, &end));
        ti_frame8(g_expect, (uint32_t)((int32_t)rec->index + k), g_cut);

        for (x = 0; x < TI_W * 3u; x++) {
            memcpy(&sample, in[k + 2] + (size_t)x * 2u, sizeof(sample));
            rec->frames_ok = rec->frames_ok && sample == g_expect[x] * 257u;
        }
    }

    for (y = begin; y < end; y++) {
        for (x = 0; x < TI_W * 3u; x++) {
            sum = 0;

            for (k = -(int32_t)rec->shot_past; k <= (int32_t)rec->shot_future; k++) {
                memcpy(&sample, in[k + 2] + (size_t)y * strides[k + 2] + (size_t)x * 2u,
                       sizeof(sample));
                sum += sample;
            }

            sample = (uint16_t)((sum + (rec->shot_past + rec->shot_future + 1u) / 2u) /
                                (rec->shot_past + rec->shot_future + 1u));
            memcpy(out + (size_t)y * out_stride + (size_t)x * 2u, &sample, sizeof(sample));
        }
    }

    return HDRG_OK;
}

/*
 * the blend filters: frames t and t + 1 mixed at the phase in integers,
 * each sample (the width the session's: g_scale's, read by the threads)
 */
static hdrg_status_t ti_blend(hdrg_job_t *job, const hdrg_plugin_api_t *api)
{
    const uint8_t *a, *b;
    hdrg_status_t status;
    uint32_t begin, end, y, x, phase, count;
    uint16_t sa, sb, mixed;
    uint8_t *out;
    size_t a_stride, b_stride, out_stride;

    status = api->job_input(job, 0, &a, &a_stride, &begin, &end);
    status =
        (status == HDRG_OK) ? api->job_input_frame(job, 1, 0, &b, &b_stride, &begin, &end) : status;
    status = (status == HDRG_OK) ? api->job_output(job, 0, &out, &out_stride) : status;
    status = (status == HDRG_OK) ? api->job_phase(job, &phase) : status;
    status = (status == HDRG_OK) ? api->job_phases(job, NULL, 0, &count) : status;
    status = (status == HDRG_OK && count == 0u) ? HDRG_ERR_PLUGIN : status;

    for (y = begin; status == HDRG_OK && y < end; y++) {
        for (x = 0; x < TI_W * g_scale * 3u; x++) {
            memcpy(&sa, a + (size_t)y * a_stride + (size_t)x * 2u, sizeof(sa));
            memcpy(&sb, b + (size_t)y * b_stride + (size_t)x * 2u, sizeof(sb));
            mixed =
                (uint16_t)(((uint32_t)sa * (65536u - phase) + (uint32_t)sb * phase + 32768u) >> 16);
            memcpy(out + (size_t)y * out_stride + (size_t)x * 2u, &mixed, sizeof(mixed));
        }
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL ti_blend_process(void *inst, hdrg_job_t *job,
                                                       const hdrg_plugin_api_t *api)
{
    (void)inst;

    return ti_blend(job, api);
}

/* the asynchronous blend: 3 jobs in flight, each ended from its thread */
static hdrg_status_t HDRG_PLUGIN_CALL ti_async_create(hdrg_filter_t *filter,
                                                      const hdrg_options_t *options, void **inst,
                                                      const hdrg_plugin_api_t *api)
{
    (void)options;
    *inst = &g_flights;

    return api->filter_set_max_inflight(filter, 3);
}

static void HDRG_PLUGIN_CALL ti_async_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    uint8_t i;

    (void)inst;
    (void)api;

    for (i = 0; i < 8u; i++) {
        if (g_flights[i].running) {
            hdrg_thread_join(&g_flights[i].thread);
            g_flights[i].running = false;
        }
    }
}

/* ... each job's end 5 ms after it starts (the next jobs submitted meanwhile) */
static void ti_async_thread(void *arg)
{
    const uint64_t until = hdrg_clock_ns() + UINT64_C(5000000);
    ti_flight_t *flight = (ti_flight_t *)arg;
    hdrg_status_t status;

    status = ti_blend(flight->job, flight->api);

    while (hdrg_clock_ns() < until) {
    }

    hdrg_mutex_lock(&g_flight_lock);
    g_flight_done++;
    hdrg_mutex_unlock(&g_flight_lock);
    (void)flight->api->job_done(flight->job, status);
}

static hdrg_status_t HDRG_PLUGIN_CALL ti_async_submit(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    ti_flight_t *flight;

    (void)inst;

    flight = &g_flights[g_flight_submits % 8u];

    if (flight->running) {
        hdrg_thread_join(&flight->thread);
        flight->running = false;
    }

    hdrg_mutex_lock(&g_flight_lock);
    g_flight_submits++;
    g_flight_peak = HDRG_TEST_MAX(g_flight_peak, g_flight_submits - g_flight_done);
    hdrg_mutex_unlock(&g_flight_lock);
    flight->job = job;
    flight->api = api;
    flight->running = hdrg_thread_create(&flight->thread, ti_async_thread, flight);

    return flight->running ? HDRG_OK : HDRG_ERR_PLUGIN;
}

static hdrg_status_t HDRG_PLUGIN_CALL ti_app_init(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, "org.test.interp", 15, "INTERP test", 11, "1.0.0", 5,
                                  "test", 4);
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, "MIT", 3, "test", 4);
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    cls = (status == HDRG_OK) ? api->class_create(plugin, "org.test.interp", 15) : NULL;
    status = (cls != NULL) ? api->class_set_stages(cls, HDRG_STAGE_INTERP) : HDRG_ERR_PLUGIN;

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, ti_app_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, ti_app_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, ti_app_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, ti_app_process);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    cls = (status == HDRG_OK) ? api->class_create(plugin, "org.test.interp.blend", 21) : NULL;
    status = (cls != NULL) ? api->class_set_stages(cls, HDRG_STAGE_INTERP) : HDRG_ERR_PLUGIN;
    status = (status == HDRG_OK) ? api->class_set_create(cls, ti_app_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, ti_app_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, ti_app_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, ti_blend_process) : status;
    status = (status == HDRG_OK) ? api->class_register(cls) : status;
    cls = (status == HDRG_OK) ? api->class_create(plugin, "org.test.interp.async", 21) : NULL;
    status = (cls != NULL) ? api->class_set_stages(cls, HDRG_STAGE_INTERP) : HDRG_ERR_PLUGIN;
    status = (status == HDRG_OK) ? api->class_set_flags(cls, HDRG_FILTER_ASYNC) : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, ti_async_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, ti_async_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, ti_app_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_submit(cls, ti_async_submit) : status;
    status = (status == HDRG_OK) ? api->class_register(cls) : status;
    cls = (status == HDRG_OK) ? api->class_create(plugin, "org.test.interp.window", 22) : NULL;
    status = (cls != NULL) ? api->class_set_stages(cls, HDRG_STAGE_INTERP) : HDRG_ERR_PLUGIN;

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, ti_window_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, ti_app_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, ti_app_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, ti_window_process);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    cls = (status == HDRG_OK) ? api->class_create(plugin, "org.test.prefilter", 18) : NULL;
    status = (cls != NULL) ? api->class_set_stages(cls, HDRG_STAGE_PREFILTER) : HDRG_ERR_PLUGIN;

    if (status == HDRG_OK) {
        status = api->class_set_create(cls, ti_pre_create);
    }

    if (status == HDRG_OK) {
        status = api->class_set_destroy(cls, ti_app_destroy);
    }

    if (status == HDRG_OK) {
        status = api->class_set_configure(cls, ti_app_configure);
    }

    if (status == HDRG_OK) {
        status = api->class_set_process(cls, ti_pre_process);
    }

    if (status == HDRG_OK) {
        status = api->class_register(cls);
    }

    return status;
}

/*
 * frames [first, last) of TI_FRAMES (frame k's pts: 1000 k) through a
 * session at in / out frames a second (out 0: none) with the INTERP filter
 * filter (NULL: the default; "app": the app's) from start with preroll;
 * the filter's or the session's failure to open returned
 */
static hdrg_status_t ti_session(uint32_t in, uint32_t out_rate, const char *filter, uint32_t cut,
                                uint32_t start, uint32_t preroll, uint32_t last, ti_run_t *run)
{
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u * g_scale * g_scale;
    hdrg_context_desc_t desc;
    hdrg_context_t *ctx;
    hdrg_session_format_t format;
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_t *session;
    hdrg_session_frame_t frame;
    hdrg_session_output_t out;
    hdrg_session_latency_t report;
    hdrg_status_t status;
    uint32_t sent, i;
    uint8_t *pixels, *at, blob[512], rgb[TI_W * TI_H * 3u];
    size_t blob_size, px;

    /* each frame in its own memory: the session reads the frames it holds where they were sent */
    px = g_rgba ? 4u : 3u;
    pixels = (uint8_t *)malloc((size_t)TI_W * TI_H * px * ((size_t)last + 1u));
    TEST_ASSERT_NOT_NULL(pixels);
    hdrg_context_desc_init(&desc);
    desc.gpu = g_gpu ? HDRG_GPU_FILTERS : HDRG_GPU_OFF;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));

    if ((filter != NULL && (strcmp(filter, "app") == 0 || strcmp(filter, "window") == 0 ||
                            strncmp(filter, "org.test.", 9) == 0)) ||
        g_prefilter) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, ti_app_init));
    }

    if (g_prefilter) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER,
                                                               "org.test.prefilter", NULL));
    }

    filter = (filter != NULL && strcmp(filter, "app") == 0) ? "org.test.interp" : filter;
    filter = (filter != NULL && strcmp(filter, "window") == 0) ? "org.test.interp.window" : filter;
    g_cut = cut;

    status =
        (filter != NULL) ? hdrg_context_add_filter(ctx, HDRG_STAGE_INTERP, filter, NULL) : HDRG_OK;

    if (status != HDRG_OK) {
        hdrg_context_destroy(ctx);
        free(pixels);

        return status;
    }

    memset(&format, 0, sizeof(format));
    format.width = TI_W;
    format.height = TI_H;
    format.bit_depth = 8;
    format.stride = (size_t)TI_W * px;
    format.has_alpha = g_rgba;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.grain_policy = HDRG_GRAIN_PRESERVE; /* the frames between alone */

    /* the window filter's frames as sent (no PREFILTER stage) */
    if (filter != NULL && strcmp(filter, "org.test.interp.window") == 0) {
        params.deblock_strength = 0.0f;
        params.deband_strength = 0.0f;
        params.mctf_strength = 0.0f;
    }

    hdrg_session_config_init(&config);
    config.output.bits = 16;
    config.dither_off = true;
    config.rate_num = in;
    config.rate_den = 1;
    config.out_rate_num = out_rate;
    config.out_rate_den = (out_rate != 0) ? 1u : 0u;
    config.start = start;
    config.preroll = preroll;
    config.output_width = (g_scale > 1u) ? TI_W * g_scale : 0u;
    config.output_height = (g_scale > 1u) ? TI_H * g_scale : 0u;

    if (g_static) {
        config.temporal = HDRG_TEMPORAL_STATIC;
        config.flash_off = true;
    }

    status = hdrg_session_create(ctx, &format, &params, NULL, &config, &session);
    if (status != HDRG_OK) {
        hdrg_context_destroy(ctx);
        free(pixels);

        return status;
    }

    sent = start;
    run->count = 0;

    for (;;) {
        memset(&out, 0, sizeof(out));
        out.planes[0] = run->frames + (size_t)run->count * frame_bytes;
        out.strides[0] = (size_t)TI_W * 6u * g_scale;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            at = pixels + (size_t)TI_W * TI_H * px * HDRG_TEST_MIN(sent, last);
            ti_frame8(g_rgba ? rgb : at, sent, cut);

            for (i = 0; g_rgba && i < TI_W * TI_H; i++) {
                memcpy(at + (size_t)i * 4u, rgb + (size_t)i * 3u, 3);
                at[(size_t)i * 4u + 3u] = 255;
            }

            memset(&frame, 0, sizeof(frame));
            frame.planes[0] = at;
            frame.pts = (int64_t)sent * 1000 + g_pts0;

            if (g_send_motion) {
                ti_motion_blob(255, (int16_t)sent, 0, blob, &blob_size);
                frame.motion = blob;
                frame.motion_size = blob_size;
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < last) ? &frame : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(run->count < TI_MAX_OUT);
        run->pts[run->count] = out.pts;
        run->src_pts[run->count][0] = out.src_pts[0];
        run->src_pts[run->count][1] = out.src_pts[1];
        run->phase[run->count] = out.phase;
        run->synthetic[run->count] = out.synthetic;
        run->max_nits[run->count] = out.max_nits;
        run->count++;
    }

    /* the delay of the session's own profile is the one it keeps (the INTERP window too) */
    report.struct_size = (uint32_t)sizeof(report);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_latency_report(session, &report));
    TEST_ASSERT_EQUAL_UINT32(hdrg_session_latency(session), report.frames);
    TEST_ASSERT_EQUAL_UINT32(report.frames, report.profiles[report.profile]);

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    free(pixels);

    return HDRG_OK;
}

static uint8_t *ti_frames(void)
{
    uint8_t *frames = (uint8_t *)malloc((size_t)TI_W * TI_H * 6u * 4u * TI_MAX_OUT);

    TEST_ASSERT_NOT_NULL(frames);

    return frames;
}

#if HDRG_BUILTIN_INTERP_MC
/*
 * width x height 16-bit R'G'B': the texture at time t (kind 0), or noise
 * moved by (37, -11) pixels a frame (kind 1: vectors far from the
 * predictors, samples that disagree)
 */
static void ti_rgb16_size(uint16_t *rgb, uint32_t width, uint32_t height, uint8_t kind, double t)
{
    uint32_t x, y, sx, sy;
    uint8_t c;
    double v;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            v = ti_texture((double)x, (double)y, t);
            sx = x + 1000u - (uint32_t)(37.0 * t);
            sy = y + 1000u + (uint32_t)(11.0 * t);

            for (c = 0; c < 3u; c++) {
                rgb[((size_t)y * width + x) * 3u + c] =
                    (kind == 0u) ? (uint16_t)(v * (0.5 + 0.2 * c) * 65535.0 + 0.5)
                                 : (uint16_t)(hdrg_hash_u32(UINT64_C(0x6d63), sx / 3u, sy / 3u, c) &
                                              0xffffu);
            }
        }
    }
}
#endif

/*
 * the GPU versions of hdrg.interp.mc (Metal, Vulkan, WebGPU: those of the
 * build with a device) give its bytes and its motion: one level, several
 * (refined and smoothed), odd sizes, phases from the first frame's to the
 * second's
 */
static void test_gpu_motion(void)
{
#if HDRG_BUILTIN_INTERP_MC
    static const uint32_t sizes[3][2] = {{96, 64}, {1283, 721}, {37, 23}},
                          phases[4] = {1, 21845, 32768, 65535};
    static const char *const ids[3] = {"hdrg.metal.interp.mc", "hdrg.vulkan.interp.mc",
                                       "hdrg.webgpu.interp.mc"};
    hdrg_status_t status;
    uint16_t *a, *b, *cpu, *gpu;
    uint8_t i, s, kind, k, ran, *motion;
    size_t samples, motion_size;
    char line[96];

    ran = 0;
    motion = (uint8_t *)malloc(TI_MOTION);
    TEST_ASSERT_NOT_NULL(motion);

    for (i = 0; i < 3u; i++) {
        status = HDRG_OK;

        for (s = 0; status == HDRG_OK && s < 3u; s++) {
            samples = (size_t)sizes[s][0] * sizes[s][1] * 3u;
            a = (uint16_t *)malloc(samples * sizeof(uint16_t));
            b = (uint16_t *)malloc(samples * sizeof(uint16_t));
            cpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
            gpu = (uint16_t *)malloc(samples * sizeof(uint16_t));
            TEST_ASSERT_TRUE(a != NULL && b != NULL && cpu != NULL && gpu != NULL);

            for (kind = 0; status == HDRG_OK && kind < 2u; kind++) {
                ti_rgb16_size(a, sizes[s][0], sizes[s][1], kind, 0.0);
                ti_rgb16_size(b, sizes[s][0], sizes[s][1], kind, 1.0);

                for (k = 0; status == HDRG_OK && k < 4u; k++) {
                    status = ti_between_size(ids[i], NULL, sizes[s][0], sizes[s][1], a, b,
                                             phases[k], gpu);

                    if (status != HDRG_OK) {
                        break;
                    }

                    motion_size = g_motion_size;
                    memcpy(motion, g_motion, motion_size);
                    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_between_size("hdrg.interp.mc", "threads=8",
                                                                   sizes[s][0], sizes[s][1], a, b,
                                                                   phases[k], cpu));
                    TEST_ASSERT_EQUAL_MEMORY(cpu, gpu, samples * sizeof(uint16_t));
                    TEST_ASSERT_EQUAL_size_t(g_motion_size, motion_size);
                    TEST_ASSERT_TRUE(motion_size > 0u);
                    TEST_ASSERT_EQUAL_MEMORY(g_motion, motion, motion_size);
                }
            }

            free(a);
            free(b);
            free(cpu);
            free(gpu);
        }

        if (status == HDRG_OK) {
            snprintf(line, sizeof(line), "%s: the CPU's bytes and motion", ids[i]);
            TEST_MESSAGE(line);
            ran++;
        } else {
            TEST_ASSERT_TRUE(status == HDRG_ERR_NOT_FOUND || status == HDRG_ERR_UNSUPPORTED);
        }
    }

    free(motion);

    if (ran == 0) {
        TEST_IGNORE_MESSAGE("no GPU version of hdrg.interp.mc with a device");
    }
#else
    TEST_IGNORE_MESSAGE("built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)");
#endif
}

/* the blend: (a (65536 - phase) + b phase + 32768) >> 16 of every sample */
static void test_blend(void)
{
    const size_t samples = (size_t)TI_W * TI_H * 3u;
    uint16_t *a, *b, *out;
    size_t i;

    a = (uint16_t *)malloc(samples * sizeof(uint16_t));
    b = (uint16_t *)malloc(samples * sizeof(uint16_t));
    out = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(a != NULL && b != NULL && out != NULL);

    ti_rgb16(a, 0.0);
    ti_rgb16(b, 1.0);
    ti_between("hdrg.interp.blend", NULL, a, b, 20000, out);

    for (i = 0; i < samples; i++) {
        TEST_ASSERT_EQUAL_UINT16(
            (uint16_t)(((uint64_t)a[i] * (65536u - 20000u) + (uint64_t)b[i] * 20000u + 32768u) >>
                       16u),
            out[i]);
    }

    ti_between("hdrg.interp.repeat", NULL, a, b, 32767, out);
    TEST_ASSERT_EQUAL_MEMORY(a, out, samples * sizeof(uint16_t));
    ti_between("hdrg.interp.repeat", NULL, a, b, 32768, out);
    TEST_ASSERT_EQUAL_MEMORY(b, out, samples * sizeof(uint16_t));

    free(a);
    free(b);
    free(out);
}

/*
 * the motion-compensated frame of a texture moving 6 x 2 pixels a frame:
 * near the truth at each phase and well above the blend, the same with one
 * thread and eight
 */
static void test_motion(void)
{
#if HDRG_BUILTIN_INTERP_MC
    static const uint32_t phases[3] = {16384, 32768, 52429};
    const uint8_t *blocks;
    const size_t samples = (size_t)TI_W * TI_H * 3u;
    hdrg_sd_motion_field_t field;
    hdrg_sd_motion_block_t block;
    uint32_t bx, by;
    uint16_t *a, *b, *truth, *mc, *blend, *one;
    uint8_t k, *motion;
    size_t motion_size;
    char line[96];
    double mc_db, blend_db;

    a = (uint16_t *)malloc(samples * sizeof(uint16_t));
    b = (uint16_t *)malloc(samples * sizeof(uint16_t));
    truth = (uint16_t *)malloc(samples * sizeof(uint16_t));
    mc = (uint16_t *)malloc(samples * sizeof(uint16_t));
    blend = (uint16_t *)malloc(samples * sizeof(uint16_t));
    one = (uint16_t *)malloc(samples * sizeof(uint16_t));
    motion = (uint8_t *)malloc(TI_MOTION);
    TEST_ASSERT_TRUE(a != NULL && b != NULL && truth != NULL && mc != NULL && blend != NULL &&
                     one != NULL && motion != NULL);

    ti_rgb16(a, 0.0);
    ti_rgb16(b, 1.0);

    for (k = 0; k < 3u; k++) {
        ti_rgb16(truth, (double)phases[k] / 65536.0);
        ti_between("hdrg.interp.mc", "threads=8", a, b, phases[k], mc);
        motion_size = g_motion_size;
        memcpy(motion, g_motion, motion_size);

        /* the motion: one field of 16 x 16 blocks, inside the texture's vector and sure */
        TEST_ASSERT_TRUE(hdrg_motion_check(motion, motion_size));
        TEST_ASSERT_TRUE(hdrg_motion_field(motion, motion_size, 0, &field, &blocks));
        TEST_ASSERT_EQUAL_UINT32(HDRG_MOTION_INTERNAL, field.source);
        TEST_ASSERT_EQUAL_INT32(1, field.ref_offset);
        TEST_ASSERT_EQUAL_UINT32(16, field.block_w);
        TEST_ASSERT_EQUAL_UINT32(TI_W / 16u, field.grid_w);
        TEST_ASSERT_EQUAL_UINT32(TI_H / 16u, field.grid_h);
        TEST_ASSERT_EQUAL_UINT32(phases[k], field.phase);
        TEST_ASSERT_EQUAL_UINT32(HDRG_MOTION_SIGNAL_LUMA, field.signal);
        TEST_ASSERT_EQUAL_UINT32(1, field.shift);

        for (by = 1; by + 1u < field.grid_h; by++) {
            for (bx = 1; bx + 1u < field.grid_w; bx++) {
                memcpy(&block, blocks + ((size_t)by * field.grid_w + bx) * sizeof(block),
                       sizeof(block));
                TEST_ASSERT_INT_WITHIN(2, (int32_t)(TI_VX * 4.0), block.mv_x);
                TEST_ASSERT_INT_WITHIN(2, (int32_t)(TI_VY * 4.0), block.mv_y);
                TEST_ASSERT_TRUE(block.confidence >= HDRG_MOTION_CAP_HI);
            }
        }

        ti_between("hdrg.interp.mc", "threads=1", a, b, phases[k], one);
        TEST_ASSERT_EQUAL_size_t(motion_size, g_motion_size);
        TEST_ASSERT_EQUAL_MEMORY(motion, g_motion, motion_size);
        ti_between("hdrg.interp.blend", NULL, a, b, phases[k], blend);
        mc_db = ti_psnr(mc, truth);
        blend_db = ti_psnr(blend, truth);
        snprintf(line, sizeof(line), "phase %u: motion compensated %.1f dB, blend %.1f dB",
                 phases[k], mc_db, blend_db);
        TEST_MESSAGE(line);

        TEST_ASSERT_TRUE(mc_db > 38.0);
        TEST_ASSERT_TRUE(mc_db > blend_db + 10.0);
        TEST_ASSERT_EQUAL_MEMORY(mc, one, samples * sizeof(uint16_t));
    }

    free(a);
    free(b);
    free(truth);
    free(mc);
    free(blend);
    free(one);
    free(motion);
#else
    TEST_IGNORE_MESSAGE("built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)");
#endif
}

/*
 * twice the rate: 2 n - 1 frames, the even ones the input frames (the same
 * bytes as at the input's rate), the odd ones halfway with their pts there
 */
static void test_twice(void)
{
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    ti_run_t base, twice;
    uint32_t j;

    base.frames = ti_frames();
    twice.frames = ti_frames();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 0, NULL, UINT32_MAX, 0, 0, TI_FRAMES, &base));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(30, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &twice));
    TEST_ASSERT_EQUAL_UINT32(TI_FRAMES, base.count);
    TEST_ASSERT_EQUAL_UINT32(2u * TI_FRAMES - 1u, twice.count);

    for (j = 0; j < twice.count; j++) {
        if (j % 2u == 0u) {
            TEST_ASSERT_FALSE(twice.synthetic[j]);
            TEST_ASSERT_EQUAL_UINT32(0, twice.phase[j]);
            TEST_ASSERT_TRUE(twice.pts[j] == (int64_t)(j / 2u) * 1000);
            TEST_ASSERT_EQUAL_MEMORY(base.frames + (size_t)(j / 2u) * frame_bytes,
                                     twice.frames + (size_t)j * frame_bytes, frame_bytes);

            continue;
        }

        TEST_ASSERT_TRUE(twice.synthetic[j]);
        TEST_ASSERT_EQUAL_UINT32(32768, twice.phase[j]);
        TEST_ASSERT_TRUE(twice.src_pts[j][0] == (int64_t)(j / 2u) * 1000);
        TEST_ASSERT_TRUE(twice.src_pts[j][1] == (int64_t)(j / 2u + 1u) * 1000);
        TEST_ASSERT_TRUE(twice.pts[j] == (int64_t)(j / 2u) * 1000 + 500);
    }

    free(base.frames);
    free(twice.frames);
}

/*
 * 24 to 60 and 60 to 24 with the repeat: every output frame the nearer input
 * frame's output (a frame with no output of its own still in the
 * statistics), the places 0, 0.4, 0.8, 0.2, 0.6
 */
static void test_rates(void)
{
    static const uint32_t places[5] = {0, 26214, 52429, 13107, 39322};
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    ti_run_t base, up, down;
    uint32_t j, nearest;

    base.frames = ti_frames();
    up.frames = ti_frames();
    down.frames = ti_frames();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(24, 0, NULL, UINT32_MAX, 0, 0, TI_FRAMES, &base));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(24, 60, "hdrg.interp.repeat", UINT32_MAX, 0, 0, TI_FRAMES, &up));
    TEST_ASSERT_EQUAL_UINT32(23, up.count); /* t = 0.4 j up to 9 */

    for (j = 0; j < up.count; j++) {
        nearest = (2u * j * 2u + 5u) / 10u;
        TEST_ASSERT_EQUAL_UINT32(places[j % 5u], up.phase[j]);
        TEST_ASSERT_EQUAL_MEMORY(base.frames + (size_t)nearest * frame_bytes,
                                 up.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    /* 60 to 24 (the base at 60: the statistics' rate) */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(60, 0, NULL, UINT32_MAX, 0, 0, TI_FRAMES, &base));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(60, 24, "hdrg.interp.repeat", UINT32_MAX, 0, 0, TI_FRAMES, &down));
    TEST_ASSERT_EQUAL_UINT32(4, down.count); /* t = 2.5 j up to 9 */

    for (j = 0; j < down.count; j++) {
        nearest = (5u * j + 1u) / 2u;
        TEST_ASSERT_EQUAL_UINT32((j % 2u == 0u) ? 0u : 32768u, down.phase[j]);
        TEST_ASSERT_EQUAL_MEMORY(base.frames + (size_t)nearest * frame_bytes,
                                 down.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    free(base.frames);
    free(up.frames);
    free(down.frames);
}

/*
 * the frames as RGBA (opaque) give the RGB frames' outputs, between the frames too; a stream
 * of negative times (B-frames' first) the same frames, its times as sent
 */
static void test_rates_rgba_and_negative(void)
{
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    ti_run_t rgb, other;
    uint32_t j;

    rgb.frames = ti_frames();
    other.frames = ti_frames();
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(24, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &rgb));
    g_rgba = true;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(24, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &other));
    g_rgba = false;
    TEST_ASSERT_EQUAL_UINT32(rgb.count, other.count);
    TEST_ASSERT_EQUAL_MEMORY(rgb.frames, other.frames, frame_bytes * rgb.count);

    g_pts0 = -5000;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(24, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &other));
    g_pts0 = 0;
    TEST_ASSERT_EQUAL_UINT32(rgb.count, other.count);
    TEST_ASSERT_EQUAL_MEMORY(rgb.frames, other.frames, frame_bytes * rgb.count);

    for (j = 0; j < rgb.count; j++) {
        TEST_ASSERT_TRUE(other.pts[j] == rgb.pts[j] - 5000);
        TEST_ASSERT_EQUAL_UINT32(rgb.phase[j], other.phase[j]);
    }

    free(rgb.frames);
    free(other.frames);
}

/* across a cut the nearer frame's output; the app's filter told the phases of the others */
static void test_cut_and_app(void)
{
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    ti_run_t base, twice, app;
    uint32_t j;

    base.frames = ti_frames();
    twice.frames = ti_frames();
    app.frames = ti_frames();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 0, NULL, 5, 0, 0, TI_FRAMES, &base));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          ti_session(30, 60, "hdrg.interp.blend", 5, 0, 0, TI_FRAMES, &twice));

    /* t = 4.5 (j 9): the head is frame 5, after the cut */
    TEST_ASSERT_TRUE(twice.synthetic[9]);
    TEST_ASSERT_EQUAL_MEMORY(base.frames + 5u * frame_bytes, twice.frames + 9u * frame_bytes,
                             frame_bytes);
    TEST_ASSERT_TRUE(
        memcmp(base.frames + 4u * frame_bytes, twice.frames + 7u * frame_bytes, frame_bytes) != 0);

    g_jobs = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 90, "app", UINT32_MAX, 0, 0, 4, &app));
    TEST_ASSERT_EQUAL_UINT32(10, app.count); /* t = j / 3 up to 3 */
    TEST_ASSERT_EQUAL_UINT32(6, g_jobs);

    for (j = 0; j < 6u; j++) {
        TEST_ASSERT_EQUAL_UINT32((j % 2u == 0u) ? 21845u : 43691u, g_phases[j]);
    }

    free(base.frames);
    free(twice.frames);
    free(app.frames);
}

/* a session opened again at frame n - preroll: its outputs from frame n the same bytes */
static void test_seek(void)
{
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    ti_run_t whole, part;
    uint32_t preroll, n, first, j;

    whole.frames = ti_frames();
    part.frames = ti_frames();
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(24, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &whole));

    /* the preroll a session at the rates asks for */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_INTERP, "hdrg.interp.blend", NULL));
    memset(&format, 0, sizeof(format));
    format.width = TI_W;
    format.height = TI_H;
    format.bit_depth = 8;
    format.stride = (size_t)TI_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_session_config_init(&config);
    config.rate_num = 24;
    config.rate_den = 1;
    config.out_rate_num = 60;
    config.out_rate_den = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));
    preroll = hdrg_session_preroll(session);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    n = 7;
    preroll = HDRG_TEST_MAX(preroll, 1u);
    preroll = (preroll > n) ? n : preroll;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(24, 60, "hdrg.interp.blend", UINT32_MAX, n - preroll,
                                              preroll, TI_FRAMES, &part));

    /* the first output at or after frame n: t = 0.4 j, its nearest frame n */
    first = (10u * n - 5u + 3u) / 4u;
    TEST_ASSERT_EQUAL_UINT32(whole.count - first, part.count);

    for (j = 0; j < part.count; j++) {
        TEST_ASSERT_TRUE(part.pts[j] == whole.pts[first + j]);
        TEST_ASSERT_EQUAL_MEMORY(whole.frames + (size_t)(first + j) * frame_bytes,
                                 part.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    free(whole.frames);
    free(part.frames);
}

/*
 * An INTERP filter's window (2 frames before t, 3 after it) at 30 -> 75
 * frames a second with a cut at frame 6: a job for every output between
 * two frames but across the cut, each with the frames sent around it (fewer
 * at the stream's ends), those of its shot up to the cut and the phases of
 * its interval's two outputs (0.4 and 0.8 of it, or 0.2 and 0.6); a
 * session opened again at frame n - its preroll (STATIC: the INTERP
 * window's past and the frame before) the same bytes from frame n
 */
static void test_window(void)
{
    const ti_window_job_t *rec;
    static const uint32_t phases[2][2] = {{26214, 52429}, {13107, 39322}};
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    hdrg_params_t params;
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    ti_run_t whole, part;
    uint32_t j, a, past, future, preroll, n, first;

    whole.frames = ti_frames();
    part.frames = ti_frames();
    g_window_count = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 75, "window", 6, 0, 0, TI_FRAMES, &whole));
    TEST_ASSERT_EQUAL_UINT32(2u * (TI_FRAMES - 2u), g_window_count);

    for (j = 0; j < g_window_count; j++) {
        rec = &g_window_jobs[j];
        a = rec->index;
        past = HDRG_TEST_MIN(2u, a);
        future = HDRG_TEST_MIN(3u, TI_FRAMES - 1u - a);
        TEST_ASSERT_TRUE(a != 5u && a < TI_FRAMES - 1u && rec->frames_ok);
        TEST_ASSERT_EQUAL_UINT32(past, rec->past);
        TEST_ASSERT_EQUAL_UINT32(future, rec->future);
        TEST_ASSERT_EQUAL_UINT32((a >= 6u) ? HDRG_TEST_MIN(past, a - 6u) : past, rec->shot_past);
        TEST_ASSERT_EQUAL_UINT32((a < 6u) ? HDRG_TEST_MIN(future, 5u - a) : future,
                                 rec->shot_future);
        TEST_ASSERT_EQUAL_UINT32(2, rec->phase_count);
        TEST_ASSERT_EQUAL_UINT32(phases[a % 2u][0], rec->phases[0]);
        TEST_ASSERT_EQUAL_UINT32(phases[a % 2u][1], rec->phases[1]);
        TEST_ASSERT_TRUE(rec->phase == rec->phases[0] || rec->phase == rec->phases[1]);
    }

    /* the seek: STATIC, no cut */
    g_static = true;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          ti_session(30, 75, "window", UINT32_MAX, 0, 0, TI_FRAMES, &whole));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, ti_app_init));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_INTERP, "org.test.interp.window", NULL));
    memset(&format, 0, sizeof(format));
    format.width = TI_W;
    format.height = TI_H;
    format.bit_depth = 8;
    format.stride = (size_t)TI_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_session_config_init(&config);
    config.rate_num = 30;
    config.rate_den = 1;
    config.out_rate_num = 75;
    config.out_rate_den = 1;
    config.temporal = HDRG_TEMPORAL_STATIC;
    config.flash_off = true;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.deblock_strength = 0.0f;
    params.deband_strength = 0.0f;
    params.mctf_strength = 0.0f;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, &params, NULL, &config, &session));
    preroll = hdrg_session_preroll(session);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_UINT32(3, preroll);

    n = 7;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(30, 75, "window", UINT32_MAX, n - preroll, preroll, TI_FRAMES, &part));
    g_static = false;

    /* the first output at or after frame n: t = 0.4 j, its nearest frame n */
    first = (10u * n - 5u + 3u) / 4u;
    TEST_ASSERT_EQUAL_UINT32(whole.count - first, part.count);

    for (j = 0; j < part.count; j++) {
        TEST_ASSERT_TRUE(part.pts[j] == whole.pts[first + j]);
        TEST_ASSERT_EQUAL_MEMORY(whole.frames + (size_t)(first + j) * frame_bytes,
                                 part.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    free(whole.frames);
    free(part.frames);
}

/*
 * SCALE and the output rate together: the input frames scaled to twice the
 * size, the frames between at that size. At 30 -> 60 frames a second the
 * input frames the same bytes as the session at the input's rate; the app's
 * filter (the second frame, a sure motion) reads the frames at the output's
 * size and its frames between are the next input frame's bytes; with the
 * SCALE the GPU's (a filter in flight, keeping frames, where the build has
 * a device) the same bytes; a seek with the preroll the same bytes
 */
static void test_scale_rate(void)
{
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u * 4u;
    ti_run_t base, run, part;
    uint32_t j, n, first;

    base.frames = ti_frames();
    run.frames = ti_frames();
    part.frames = ti_frames();
    g_scale = 2;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 0, NULL, UINT32_MAX, 0, 0, TI_FRAMES, &base));
    TEST_ASSERT_EQUAL_UINT32(TI_FRAMES, base.count);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(30, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &run));
    TEST_ASSERT_EQUAL_UINT32(2u * TI_FRAMES - 1u, run.count);

    for (j = 0; j < run.count; j++) {
        TEST_ASSERT_TRUE(run.synthetic[j] == (j % 2u == 1u));

        if (j % 2u == 0u) {
            TEST_ASSERT_EQUAL_MEMORY(base.frames + (size_t)(j / 2u) * frame_bytes,
                                     run.frames + (size_t)j * frame_bytes, frame_bytes);
        }
    }

    /* the app's filter: the frames at the output's size; the second frame with a sure motion */
    g_app_mode = 1;
    g_app_conf = 255;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 60, "app", UINT32_MAX, 0, 0, TI_FRAMES, &run));
    g_app_mode = 0;
    TEST_ASSERT_EQUAL_UINT32(TI_W * 2u, g_app_w);

    for (j = 1; j < run.count; j += 2u) {
        TEST_ASSERT_EQUAL_MEMORY(run.frames + (size_t)(j + 1u) * frame_bytes,
                                 run.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    /* a SCALE keeping frames (the GPU's Lanczos in flight: the CPU's bytes) */
    g_gpu = true;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(30, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &part));
    g_gpu = false;
    TEST_ASSERT_EQUAL_UINT32(run.count, part.count);

    for (j = 0; j < part.count; j += 2u) {
        TEST_ASSERT_EQUAL_MEMORY(base.frames + (size_t)(j / 2u) * frame_bytes,
                                 part.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    /* the seek: STATIC, its preroll the frame before and the MCTF's past (2) */
    g_static = true;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(30, 60, "hdrg.interp.blend", UINT32_MAX, 0, 0, TI_FRAMES, &run));
    n = 6;
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, ti_session(30, 60, "hdrg.interp.blend", UINT32_MAX, n - 3u, 3, TI_FRAMES, &part));
    g_static = false;
    first = 2u * n - 1u;
    TEST_ASSERT_EQUAL_UINT32(run.count - first, part.count);

    for (j = 0; j < part.count; j++) {
        TEST_ASSERT_TRUE(part.pts[j] == run.pts[first + j]);
        TEST_ASSERT_EQUAL_MEMORY(run.frames + (size_t)(first + j) * frame_bytes,
                                 part.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    free(base.frames);
    free(run.frames);
    free(part.frames);
}

/*
 * An INTERP filter keeping 3 jobs in flight (ASYNC, each ended from a
 * thread of its own): the session submits the jobs of the outputs ahead
 * (more than one in flight at once, but under Valgrind, whose threads run
 * one at a time), each with the window it would have
 * alone, and its outputs are the same bytes as the same blend run job by
 * job; at 30 -> 75 frames a second with a cut, at the input's size and
 * with SCALE twice
 */
static void test_inflight(void)
{
    ti_run_t sync, async;
    uint32_t s, j;
    size_t frame_bytes;

    HDRG_TEST_REQUIRE_THREADS();

    sync.frames = ti_frames();
    async.frames = ti_frames();
    TEST_ASSERT_TRUE(hdrg_mutex_init(&g_flight_lock));

    for (s = 1; s <= 2u; s++) {
        g_scale = s;
        frame_bytes = (size_t)TI_W * TI_H * 6u * s * s;
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, ti_session(30, 75, "org.test.interp.blend", 6, 0, 0, TI_FRAMES, &sync));
        g_flight_submits = 0;
        g_flight_done = 0;
        g_flight_peak = 0;
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, ti_session(30, 75, "org.test.interp.async", 6, 0, 0, TI_FRAMES, &async));
        TEST_ASSERT_EQUAL_UINT32(2u * (TI_FRAMES - 2u), g_flight_submits);
#if !defined(HDRG_TEST_VALGRIND)
        TEST_ASSERT_TRUE(g_flight_peak >= 2u);
#endif
        TEST_ASSERT_EQUAL_UINT32(sync.count, async.count);

        for (j = 0; j < sync.count; j++) {
            TEST_ASSERT_TRUE(sync.pts[j] == async.pts[j] &&
                             sync.synthetic[j] == async.synthetic[j]);
            TEST_ASSERT_EQUAL_MEMORY(sync.frames + (size_t)j * frame_bytes,
                                     async.frames + (size_t)j * frame_bytes, frame_bytes);
        }
    }

    g_scale = 1;
    hdrg_mutex_destroy(&g_flight_lock);
    free(sync.frames);
    free(async.frames);
}

/* a blob of one field of 2 x 1 blocks of 8 x 8 pixels at phase 1/2 (the blocks' conf, mv_x) */
static size_t ti_two_blocks(uint8_t *blob, const uint8_t *conf, const int16_t *mv_x)
{
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field;
    hdrg_sd_motion_block_t block;
    uint8_t i;
    size_t size;

    memset(&head, 0, sizeof(head));
    memset(&field, 0, sizeof(field));
    head.fields = 1;
    field.source = HDRG_MOTION_HOST;
    field.ref_offset = 1;
    field.block_w = 8;
    field.block_h = 8;
    field.grid_w = 2;
    field.grid_h = 1;
    field.phase = 32768;
    field.signal = HDRG_MOTION_SIGNAL_NONE;
    memcpy(blob, &head, sizeof(head));
    memcpy(blob + sizeof(head), &field, sizeof(field));
    size = sizeof(head) + sizeof(field);

    for (i = 0; i < 2u; i++) {
        memset(&block, 0, sizeof(block));
        block.confidence = conf[i];
        block.mv_x = mv_x[i];
        memcpy(blob + size, &block, sizeof(block));
        size += sizeof(block);
    }

    return size;
}

/*
 * HDRG_SD_MOTION's layout (each field's header, its blocks, the size) and
 * the gain cap: an unsure block's cell at most the smaller of the grids at
 * its places in the two frames (none at all, in proportion between), a
 * sure one as it was
 */
static void test_motion_layout(void)
{
    static const uint8_t sure[2] = {255, 255}, unsure[2] = {0, 255}, half[2] = {128, 255};
    static const int16_t still[2] = {0, 0}, moved[2] = {32, 0};
    hdrg_sd_motion_t head;
    hdrg_sd_motion_field_t field, bad;
    hdrg_plan_t plan;
    uint32_t i;
    uint8_t blob[256], copy[256], occluded;
    size_t size;
    float boost[2u * HDRG_PLAN_BINS], a[2u * HDRG_PLAN_BINS], b[2u * HDRG_PLAN_BINS],
        capped[2u * HDRG_PLAN_BINS];

    size = ti_two_blocks(blob, sure, still);
    TEST_ASSERT_TRUE(hdrg_motion_check(blob, size));
    TEST_ASSERT_FALSE(hdrg_motion_check(blob, size - 1u));
    /* shorter than the header, a field cut short */
    TEST_ASSERT_FALSE(hdrg_motion_check(blob, sizeof(hdrg_sd_motion_t) - 1u));
    TEST_ASSERT_FALSE(
        hdrg_motion_check(blob, sizeof(hdrg_sd_motion_t) + sizeof(hdrg_sd_motion_field_t) - 1u));
    TEST_ASSERT_FALSE(hdrg_motion_check(NULL, size));
    memcpy(copy, blob, size);
    copy[size] = 0;
    TEST_ASSERT_FALSE(hdrg_motion_check(copy, size + 1u));

    /* the header's fields and reserved, each field's values, a block's occlusion */
    memcpy(&head, blob, sizeof(head));
    memcpy(&field, blob + sizeof(head), sizeof(field));

    for (i = 0; i < 18u; i++) {
        memcpy(copy, blob, size);
        bad = field;
        head.fields = (i == 0u) ? 0u : (i == 1u) ? HDRG_SD_MOTION_FIELDS + 1u : 1u;
        head.reserved = (i == 2u) ? 1u : 0u;
        bad.source = (i == 3u) ? 0u : (i == 4u) ? HDRG_MOTION_HOST + 1u : bad.source;
        bad.ref_offset = (i == 5u) ? 0 : bad.ref_offset;
        bad.block_w = (i == 6u) ? 0u : (i == 7u) ? HDRG_MOTION_BLOCK_MAX + 1u : bad.block_w;
        bad.grid_h = (i == 8u) ? 0u : (i == 17u) ? (1u << 24) : bad.grid_h;
        bad.block_h = (i == 14u) ? 0u : (i == 15u) ? HDRG_MOTION_BLOCK_MAX + 1u : bad.block_h;
        bad.grid_w = (i == 16u) ? 0u : (i == 17u) ? 2u : bad.grid_w;
        bad.phase = (i == 9u) ? 65537u : bad.phase;
        bad.signal = (i == 10u) ? 0u : (i == 11u) ? HDRG_MOTION_SIGNAL_NONE + 1u : bad.signal;
        bad.shift = (i == 12u) ? 9u : bad.shift;
        bad.reserved = (i == 13u) ? 1u : 0u;
        memcpy(copy, &head, sizeof(head));
        memcpy(copy + sizeof(head), &bad, sizeof(bad));
        TEST_ASSERT_FALSE(hdrg_motion_check(copy, size));
    }

    memcpy(copy, blob, size);
    occluded = HDRG_MOTION_UNCOVERED + 1u;
    memcpy(copy + sizeof(head) + sizeof(field) + 7u, &occluded, 1);
    TEST_ASSERT_FALSE(hdrg_motion_check(copy, size));

    /* a plan of 2 x 1 cells of 8 pixels: boost 0.8, the first frame's 0.2 / 0.6, the second's
       0.5 / 0.1 */
    memset(&plan, 0, sizeof(plan));
    plan.grid_w = 2;
    plan.grid_h = 1;
    plan.cell = 8;
    plan.width = 16;
    plan.height = 8;
    plan.boost = boost;

    for (i = 0; i < HDRG_PLAN_BINS; i++) {
        boost[i] = 0.8f;
        boost[HDRG_PLAN_BINS + i] = 0.8f;
        a[i] = 0.2f;
        a[HDRG_PLAN_BINS + i] = 0.6f;
        b[i] = 0.5f;
        b[HDRG_PLAN_BINS + i] = 0.1f;
    }

    size = ti_two_blocks(blob, sure, still);
    TEST_ASSERT_FALSE(hdrg_motion_cap(&plan, a, b, blob, size, 16, 8, capped));
    TEST_ASSERT_EQUAL_MEMORY(boost, capped, sizeof(boost));

    size = ti_two_blocks(blob, unsure, still);
    TEST_ASSERT_TRUE(hdrg_motion_cap(&plan, a, b, blob, size, 16, 8, capped));
    HDRG_TEST_NEAR(1e-6, 0.2, capped[3]);
    HDRG_TEST_NEAR(1e-6, 0.8, capped[HDRG_PLAN_BINS + 3u]);

    /* halfway sure: half the cap; the first frame's grid read a cell to the left of the second's
       (8 pixels at phase 1/2: -4 and +4 from the cell's center) */
    size = ti_two_blocks(blob, half, still);
    TEST_ASSERT_TRUE(hdrg_motion_cap(&plan, a, b, blob, size, 16, 8, capped));
    HDRG_TEST_NEAR(1e-6, 0.5, capped[3]);

    size = ti_two_blocks(blob, unsure, moved);
    TEST_ASSERT_TRUE(hdrg_motion_cap(&plan, a, b, blob, size, 16, 8, capped));
    HDRG_TEST_NEAR(1e-6, 0.1, capped[3]);

    /* a frame without boost: none */
    size = ti_two_blocks(blob, unsure, still);
    TEST_ASSERT_TRUE(hdrg_motion_cap(&plan, NULL, b, blob, size, 16, 8, capped));
    HDRG_TEST_NEAR(1e-6, 0.0, capped[3]);
}

/*
 * the app's frames' motion: each INTERP job told its first frame's, each
 * PREFILTER frame (a window: the frames held) its own; job_set_side_data
 * refuses another type (UNSUPPORTED), version and a broken layout
 */
static void test_motion_transport(void)
{
    ti_run_t run;
    uint32_t j;

    run.frames = ti_frames();
    g_send_motion = true;
    g_prefilter = true;
    g_jobs = 0;
    g_pre_count = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 90, "app", UINT32_MAX, 0, 0, 4, &run));
    TEST_ASSERT_EQUAL_UINT32(6, g_jobs);
    TEST_ASSERT_EQUAL_UINT32(4, g_pre_count);

    for (j = 0; j < 6u; j++) {
        TEST_ASSERT_EQUAL_INT32((int32_t)(j / 2u), g_seen_mvs[j]);
    }

    for (j = 0; j < 4u; j++) {
        TEST_ASSERT_EQUAL_INT32((int32_t)j, g_pre_seen[j]);
    }

    /* without: none of the app's told (hdrg.mctf's alone, after the first frame) */
    g_send_motion = false;
    g_prefilter = false;
    g_jobs = 0;
    g_mctf_fields = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 90, "app", UINT32_MAX, 0, 0, 4, &run));
    TEST_ASSERT_EQUAL_INT32(-1, g_seen_mvs[0]);
    TEST_ASSERT_TRUE(g_mctf_fields > 0u);

    g_app_mode = 1;
    g_app_conf = 255;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 60, "app", UINT32_MAX, 0, 0, 3, &run));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, g_refusals[0]);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_refusals[1]);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_refusals[2]);
    g_app_mode = 0;

    free(run.frames);
}

/*
 * the gain cap of a frame between: a lamp that jumps
 * further than the search (hdrg.interp.mc unsure there) is dimmer between
 * than in both frames; a lamp it follows as bright; the app's filter giving
 * the second frame: with a sure motion its bytes, an unsure one dimmer
 */
static void test_cap(void)
{
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    ti_run_t run;
    uint32_t j;
    char line[128];
    float lower;

    run.frames = ti_frames();

#if HDRG_BUILTIN_INTERP_MC
    g_content = 2;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 60, NULL, UINT32_MAX, 0, 0, 6, &run));
    TEST_ASSERT_EQUAL_UINT32(11, run.count);

    for (j = 1; j < run.count; j += 2u) {
        lower = HDRG_MIN(run.max_nits[j - 1u], run.max_nits[j + 1u]);
        snprintf(line, sizeof(line), "lamp followed, frame %u: %.1f between %.1f and %.1f nits", j,
                 (double)run.max_nits[j], (double)run.max_nits[j - 1u],
                 (double)run.max_nits[j + 1u]);
        TEST_MESSAGE(line);
        TEST_ASSERT_TRUE(run.max_nits[j] >= 0.95f * lower);
    }

    g_content = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 60, NULL, UINT32_MAX, 0, 0, 6, &run));

    for (j = 1; j < run.count; j += 2u) {
        lower = HDRG_MIN(run.max_nits[j - 1u], run.max_nits[j + 1u]);
        snprintf(line, sizeof(line), "lamp jumping, frame %u: %.1f between %.1f and %.1f nits", j,
                 (double)run.max_nits[j], (double)run.max_nits[j - 1u],
                 (double)run.max_nits[j + 1u]);
        TEST_MESSAGE(line);
        TEST_ASSERT_TRUE(run.max_nits[j] < 0.9f * lower);
    }
#else
    (void)line;
    (void)lower;
#endif

    /* t = k + 1/2: the second frame nearer, its plan bound */
    g_content = 1;
    g_app_mode = 1;
    g_app_conf = 255;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 60, "app", UINT32_MAX, 0, 0, 6, &run));

    for (j = 1; j < run.count; j += 2u) {
        TEST_ASSERT_EQUAL_MEMORY(run.frames + (size_t)(j + 1u) * frame_bytes,
                                 run.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    g_app_conf = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 60, "app", UINT32_MAX, 0, 0, 6, &run));

    for (j = 1; j < run.count; j += 2u) {
        TEST_ASSERT_TRUE(run.max_nits[j] < 0.9f * run.max_nits[j + 1u]);
    }

    g_app_mode = 0;
    g_content = 0;
    free(run.frames);
}

/*
 * the cap the same whatever the device of hdrg.interp.mc (its GPU
 * versions' sessions the CPU's bytes) and after a seek with the preroll
 */
static void test_cap_seek(void)
{
#if HDRG_BUILTIN_INTERP_MC
    const size_t frame_bytes = (size_t)TI_W * TI_H * 6u;
    static const char *const ids[3] = {"hdrg.metal.interp.mc", "hdrg.vulkan.interp.mc",
                                       "hdrg.webgpu.interp.mc"};
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    ti_run_t whole, part;
    uint32_t j, n, preroll;
    uint8_t i;

    whole.frames = ti_frames();
    part.frames = ti_frames();
    g_content = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          ti_session(30, 60, "hdrg.interp.mc", UINT32_MAX, 0, 0, TI_SEEK, &whole));

    for (i = 0; i < 3u; i++) {
        status = ti_session(30, 60, ids[i], UINT32_MAX, 0, 0, TI_SEEK, &part);
        if (status != HDRG_OK) {
            TEST_ASSERT_TRUE(status == HDRG_ERR_NOT_FOUND || status == HDRG_ERR_UNSUPPORTED);

            continue;
        }

        TEST_ASSERT_EQUAL_UINT32(whole.count, part.count);
        TEST_ASSERT_EQUAL_MEMORY(whole.frames, part.frames, (size_t)whole.count * frame_bytes);
    }

    /* the preroll a session at the rates asks for */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    memset(&format, 0, sizeof(format));
    format.width = TI_W;
    format.height = TI_H;
    format.bit_depth = 8;
    format.stride = (size_t)TI_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_session_config_init(&config);
    config.rate_num = 30;
    config.rate_den = 1;
    config.out_rate_num = 60;
    config.out_rate_den = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));
    preroll = hdrg_session_preroll(session);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    /* from frame n: the outputs whose nearest frame is n or later (t = n - 1/2 on, j = 2 n - 1) */
    n = TI_SEEK - 6u;
    TEST_ASSERT_TRUE(preroll < n);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, ti_session(30, 60, "hdrg.interp.mc", UINT32_MAX, n - preroll,
                                              preroll, TI_SEEK, &part));
    TEST_ASSERT_EQUAL_UINT32(whole.count - (2u * n - 1u), part.count);
    TEST_ASSERT_TRUE(part.synthetic[0]);

    for (j = 0; j < part.count; j++) {
        TEST_ASSERT_EQUAL_MEMORY(whole.frames + (size_t)(2u * n - 1u + j) * frame_bytes,
                                 part.frames + (size_t)j * frame_bytes, frame_bytes);
    }

    g_content = 0;
    free(whole.frames);
    free(part.frames);
#else
    TEST_IGNORE_MESSAGE("built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)");
#endif
}

/* without the input's rate, or with a SCALE stage; a frame's broken motion */
static void test_rejected(void)
{
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_session_t *session;
    hdrg_session_frame_t frame;
    hdrg_context_t *ctx;
    static uint8_t pixels[TI_W * TI_H * 3u];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    memset(&format, 0, sizeof(format));
    format.width = TI_W;
    format.height = TI_H;
    format.bit_depth = 8;
    format.stride = (size_t)TI_W * 3u;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    hdrg_session_config_init(&config);
    config.out_rate_num = 60;
    config.out_rate_den = 1;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));

    config.rate_num = 30;
    config.out_rate_den = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));

    config.rate_den = 1;
    config.out_rate_den = 1;

#if !HDRG_BUILTIN_INTERP_MC
    config.output_width = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));
#endif

    config.output_width = 0;
    config.out_rate_num = 0;
    config.out_rate_den = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, NULL, &config, &session));
    memset(pixels, 0, sizeof(pixels));
    memset(&frame, 0, sizeof(frame));
    frame.planes[0] = pixels;
    frame.motion_size = 8;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &frame));
    frame.motion = pixels;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_send_frame(session, &frame));
    hdrg_session_destroy(session);

    hdrg_context_destroy(ctx);
}

void setUp(void)
{
    g_static = false;
    g_scale = 1;
    g_gpu = false;
    g_rgba = false;
    g_pts0 = 0;
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blend);
    RUN_TEST(test_motion);
    RUN_TEST(test_gpu_motion);
    RUN_TEST(test_twice);
    RUN_TEST(test_rates);
    RUN_TEST(test_rates_rgba_and_negative);
    RUN_TEST(test_cut_and_app);
    RUN_TEST(test_seek);
    RUN_TEST(test_window);
    RUN_TEST(test_scale_rate);
    RUN_TEST(test_inflight);
    RUN_TEST(test_motion_layout);
    RUN_TEST(test_motion_transport);
    RUN_TEST(test_cap);
    RUN_TEST(test_cap_seek);
    RUN_TEST(test_rejected);

    return UNITY_END();
}
