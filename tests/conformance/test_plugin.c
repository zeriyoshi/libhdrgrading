/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <unity.h>

#include <math.h>

#include "../test_stage.h"
#include "../test_util.h"
#include "core/noise.h"
#include "hdrgrading/filter.h"
#include "hdrgrading/foundation/gpu_share.h"
#include "hdrgrading/video.h"
#include "io/io.h"
#include "host/format.h"
#include "host/stage.h"
#include "priv.h"
#include "hdrgrading/foundation/hash.h"
#include "hdrgrading/foundation/thread.h"

/*
 * フィルタプラグイン: 不正な報告は API 表の呼び出しの中で
 * 拒否され、凍結後の登録は効かない。方針で拒否でき、新しすぎる ABI のモジュールは
 * 入口関数を呼ばずに拒否される。オプションは宣言した型と範囲で検証される。SCALE 段の
 * 出力は帯の分け方・スレッド数・帯 / 全画面の処理方式によらずバイト一致する。
 */
#define TP_LIT(s)  s, sizeof(s) - 1u
#define TP_W       96u
#define TP_H       64u
#define TP_FRAMES  3u  /* the asynchronous filter's session */
#define TP_WFRAMES 8u  /* the time window's sessions */
#define TP_JOBS    32u /* the asynchronous plugin's jobs it records */
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define TP_THREADS 0 /* no threads: jobs end within submit alone */
#else
#define TP_THREADS 1
#endif
#define TP_DN_W    256u /* the noise reduction's frames */
#define TP_DN_H    128u
#define TP_DN_KEY  UINT64_C(0x64656e6f697365)
#define TP_DB_KEY  UINT64_C(0x6465626c6f636b)
#define TP_DB_EDGE 128u   /* the deblocking frames' real edge, on the grid */
#define TP_DF_LUMA 37296  /* the defringing frame's orange: Y' ... */
#define TP_DF_DB   -24296 /* ... B' - Y' ... */
#define TP_DF_DR   22704  /* ... and R' - Y' */
#if HDRG_BUILTIN_INTERP_MC
#define TP_INTERP_MC 1u /* hdrg.interp.mc */
#else
#define TP_INTERP_MC 0u
#endif
#if HDRG_BUILTIN_SCALE_TEMPORAL
#define TP_TEMPORAL_SR 1u /* hdrg.scale.temporal */
#else
#define TP_TEMPORAL_SR 0u
#endif
#if HDRG_BUILTIN_STATS
#define TP_STATS 1u /* hdrg.stats.temporal */
#else
#define TP_STATS 0u
#endif
/* the optional built-in plugins the build has (HDRG_PLUGINS_EXCLUDE): 1 or 0 */
#define TP_HAS(name) ((uint32_t)(HDRG_BUILTIN_##name != 0))
/* the built-in codecs' plugins: hdrg.jpeg, and png, gif, jxl where the build has them */
#if HDRG_BUILTIN_JXL
#define TP_JXL 1u
#else
#define TP_JXL 0u
#endif
#define TP_CODECS (1u + TP_HAS(PNG) + TP_HAS(GIF) + TP_JXL)
#if defined(HDRG_WITH_METAL)
#define TP_METALFX 1u /* hdrg.metalfx, after the filters' plugins */
#else
#define TP_METALFX 0u
#endif
/* the prefilters with a GPU version: the noise reduction, deblock, dering, defringe, deband */
#define TP_PREFILTERS                                                                              \
    (TP_HAS(DENOISE) + TP_HAS(DEBLOCK) + TP_HAS(DERING) + TP_HAS(DEFRINGE) + TP_HAS(DEBAND))
/* the GPU classes of the filters' plugins on each backend: <backend>.lanczos and so on */
#define TP_BACKEND_CLASSES                                                                         \
    (1u + TP_PREFILTERS + TP_HAS(MCTF) + TP_HAS(FSR) + TP_HAS(NIS) + TP_HAS(EDGE) + TP_INTERP_MC)
#if defined(HDRG_WITH_WEBGPU)
#define TP_WEBGPU_CLASSES TP_BACKEND_CLASSES
#else
#define TP_WEBGPU_CLASSES 0u
#endif
#if defined(HDRG_WITH_METAL)
#define TP_METAL_CLASSES (TP_BACKEND_CLASSES + 1u) /* and hdrg.metal.metalfx */
#else
#define TP_METAL_CLASSES 0u
#endif
#if defined(HDRG_WITH_VULKAN)
#define TP_VULKAN_CLASSES TP_BACKEND_CLASSES
#else
#define TP_VULKAN_CLASSES 0u
#endif
#define TP_BACKENDS                                                                                \
    ((uint32_t)(TP_WEBGPU_CLASSES != 0u) + (uint32_t)(TP_METAL_CLASSES != 0u) +                    \
     (uint32_t)(TP_VULKAN_CLASSES != 0u)) /* the GPU backends built */
/* the filters' plugins (polyphase always) ... */
#define TP_FILTER_PLUGINS                                                                          \
    (1u + TP_HAS(BILINEAR) + TP_PREFILTERS + TP_HAS(MCTF) + TP_HAS(INTERP) + TP_HAS(FSR) +         \
     TP_HAS(NIS) + TP_HAS(EDGE) + TP_HAS(PREDICT) + TP_STATS)
#define TP_BUILTINS                                                                                \
    (TP_FILTER_PLUGINS + TP_METALFX + TP_CODECS) /* ... the built-in plugins ...                   \
                                                  */
#define TP_CLASSES                                                                                 \
    (2u + TP_HAS(BILINEAR) + TP_PREFILTERS + TP_HAS(MCTF) + 2u * TP_HAS(INTERP) + TP_HAS(FSR) +    \
     TP_HAS(NIS) + TP_HAS(EDGE) + TP_HAS(PREDICT) + TP_STATS + TP_INTERP_MC + TP_TEMPORAL_SR +     \
     TP_WEBGPU_CLASSES + TP_METAL_CLASSES + TP_VULKAN_CLASSES) /* ... and their filters */

static hdrg_allocator_t g_alloc;
static uint32_t g_case;
static hdrg_status_t g_api_status;
static hdrg_plugin_t *g_saved_plugin;
static const hdrg_plugin_api_t *g_saved_api;
static hdrg_class_t *g_saved_class;
static const char *g_license = "MIT";
static const char *g_id = "org.test.good";
static uint8_t *g_pixels;
static char g_big[65537];

/* what the lifetime plugin saw */
typedef struct {
    uint32_t creates, destroys, configures, transfer, bits;
    void *user;
    size_t needed;
    double xy[8];
    char model[64];
} tp_life_t;

/* a job the asynchronous plugin ends from a thread of its own */
typedef struct {
    hdrg_thread_t thread;
    hdrg_job_t *job;
    const hdrg_plugin_api_t *api;
    bool running;
} tp_flight_t;

/*
 * The asynchronous plugin (mode: 0 = job_done within submit, 1 = from a
 * thread, 2 = job_done with a failure, 3 = submit fails, 4 = from a thread
 * once canceled, with a failure; depth: its max_inflight, 0: not set) and what its calls
 * returned (inflight: max_inflight 0 and 9
 * in create, any in configure); ins / outs: the first TP_JOBS jobs' frames, in the order
 * submitted (by any instance)
 */
typedef struct {
    tp_flight_t flights[8];
    const uint8_t *ins[TP_JOBS];
    uint8_t *outs[TP_JOBS];
    hdrg_mutex_t lock; /* mode 4: jobs end when canceled */
    hdrg_cond_t wake;
    hdrg_filter_t *filter;
    hdrg_status_t again, not_async, inflight[3];
    uint32_t submits, width, cancels;
    int64_t mode, depth;
    bool canceled, waiting; /* waiting: lock and wake made */
} tp_async_t;

/* an instance of the time window plugin (a mean over the window or the clip) */
typedef struct {
    hdrg_filter_t *filter;
    uint32_t width, run; /* run: the SEQUENTIAL class's jobs since the clip's first */
    int64_t past, future, clip, overlap;
    bool record; /* the whole-frame class: g_windows records its jobs */
} tp_window_t;

/*
 * ... and what its calls returned; the whole-frame jobs' frame indices,
 * windows and shots (past << 4 | future) and job_shot's last result
 */
typedef struct {
    hdrg_status_t bad, late, beyond, shot;
    hdrg_status_t plane_in, plane_out, plane_frame, handle_beyond, device_beyond, side_query;
    size_t side_needed;
    int64_t pts[32];
    uint32_t jobs, index[32], flags[32];
    uint8_t seen[32], shots[32];
} tp_windows_t;

/* an instance of the format plugin: its jobs copy (or average over past frames) in one format */
typedef struct {
    uint32_t width, height;
    int64_t in, out, past;
    int64_t fail; /* its own frames: 1 = not made, 2 = strides short, 3 = the jobs checked,
                     4 = an odd stride, 5 = planes off their samples' alignment */
} tp_format_t;

/* a frame the format plugin makes for its jobs: planes of strides past a packed row */
typedef struct {
    void *block;
    uint8_t *planes[3];
    size_t strides[3];
} tp_frame_t;

/* the frames it made and freed, and the jobs whose handles were not their frames */
typedef struct {
    uint32_t made, freed, mismatched, windows;
} tp_frames_t;

/*
 * the device plugin: its frames "on the device" are host memory of its own
 * (tight RGB16), its jobs invert; what the jobs took and gave, and the app's
 * device it saw
 */
typedef struct {
    void *device, *queue;
    uint32_t width, host_in, host_out, device_in, device_out, released;
} tp_devices_t;

static tp_life_t g_life;
static tp_devices_t g_devices;
static tp_frames_t g_frames;
static tp_async_t g_async;
static tp_windows_t g_windows;
static int g_life_marker;

/* PHOTO's params without the temporal noise reduction (hdrg.mctf: 2 frames of delay of its own) */
static const hdrg_params_t *tp_plain(void)
{
    static hdrg_params_t params;

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.mctf_strength = 0.0f;
    params.grain_policy = HDRG_GRAIN_PRESERVE;

    return &params;
}

static hdrg_status_t HDRG_PLUGIN_CALL stub_create(hdrg_filter_t *filter,
                                                  const hdrg_options_t *options, void **inst,
                                                  const hdrg_plugin_api_t *api)
{
    (void)filter;
    (void)options;
    (void)api;

    *inst = NULL;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL stub_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;
}

static hdrg_status_t HDRG_PLUGIN_CALL stub_configure(void *inst, hdrg_negotiation_t *nego,
                                                     const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)nego;
    (void)api;

    return HDRG_ERR_UNSUPPORTED;
}

static hdrg_status_t HDRG_PLUGIN_CALL stub_process(void *inst, hdrg_job_t *job,
                                                   const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)job;
    (void)api;

    return HDRG_ERR_UNSUPPORTED;
}

static hdrg_status_t HDRG_PLUGIN_CALL life_create(hdrg_filter_t *filter,
                                                  const hdrg_options_t *options, void **inst,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    g_life.creates++;
    *inst = &g_life;

    status = api->filter_plugin_user(filter, &g_life.user);
    if (status == HDRG_OK) {
        status = api->option_get_string(options, TP_LIT("model"), NULL, 0, &g_life.needed);
    }

    if (status == HDRG_OK && g_life.needed <= sizeof(g_life.model)) {
        status = api->option_get_string(options, TP_LIT("model"), g_life.model,
                                        sizeof(g_life.model), &g_life.needed);
    }

    return status;
}

static void HDRG_PLUGIN_CALL life_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)api;

    ((tp_life_t *)inst)->destroys++;
}

static hdrg_status_t HDRG_PLUGIN_CALL life_configure(void *inst, hdrg_negotiation_t *nego,
                                                     const hdrg_plugin_api_t *api)
{
    tp_life_t *life = (tp_life_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format;

    life->configures++;

    status = api->nego_input(nego, &width, &height, &format);
    if (status == HDRG_OK) {
        status = api->nego_input_color(nego, life->xy, &life->transfer, &life->bits);
    }

    return (status == HDRG_OK) ? api->nego_set_output(nego, width, height, format) : status;
}

/* the input rows as they are */
static hdrg_status_t HDRG_PLUGIN_CALL life_process(void *inst, hdrg_job_t *job,
                                                   const hdrg_plugin_api_t *api)
{
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y;
    uint8_t *out;
    size_t in_stride, out_stride;

    (void)inst;

    status = api->job_rows(job, &begin, &end);
    status =
        (status == HDRG_OK) ? api->job_input(job, 0, &in, &in_stride, &in_begin, &in_end) : status;
    status = (status == HDRG_OK) ? api->job_output(job, 0, &out, &out_stride) : status;

    for (y = begin; status == HDRG_OK && y < end; y++) {
        memcpy(out + (size_t)(y - begin) * out_stride, in + (size_t)(y - in_begin) * in_stride,
               out_stride);
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL life_init(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.life"), TP_LIT("Lifetime"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? api->plugin_set_user(plugin, &g_life_marker) : status;
    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, TP_LIT("org.test.life"));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    status = (status == HDRG_OK)
                 ? api->class_set_flags(cls, HDRG_FILTER_BANDS | HDRG_FILTER_DETERMINISTIC)
                 : status;
    status = (status == HDRG_OK)
                 ? api->class_add_option_string(cls, TP_LIT("model"), TP_LIT("none.onnx"),
                                                TP_LIT("the model's file"))
                 : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, life_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, life_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, life_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, life_process) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

/* the inverted input (a whole frame) */
static hdrg_status_t async_invert(hdrg_job_t *job, const hdrg_plugin_api_t *api)
{
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, y, x;
    uint16_t sample;
    uint8_t *out;
    size_t in_stride, out_stride;

    status = api->job_rows(job, &begin, &end);
    status =
        (status == HDRG_OK) ? api->job_input(job, 0, &in, &in_stride, &in_begin, &in_end) : status;
    status = (status == HDRG_OK) ? api->job_output(job, 0, &out, &out_stride) : status;

    for (y = begin; status == HDRG_OK && y < end; y++) {
        for (x = 0; x < g_async.width * 3u; x++) {
            memcpy(&sample, in + (size_t)(y - in_begin) * in_stride + x * sizeof(uint16_t),
                   sizeof(sample));
            sample = (uint16_t)(65535u - sample);
            memcpy(out + (size_t)(y - begin) * out_stride + x * sizeof(uint16_t), &sample,
                   sizeof(sample));
        }
    }

    return status;
}

static void async_thread(void *arg)
{
    tp_flight_t *flight = (tp_flight_t *)arg;

    /* mode 4: the job ends once canceled */
    if (g_async.mode == 4) {
        hdrg_mutex_lock(&g_async.lock);

        while (!g_async.canceled) {
            hdrg_cond_wait(&g_async.wake, &g_async.lock);
        }

        hdrg_mutex_unlock(&g_async.lock);

        (void)flight->api->job_done(flight->job, HDRG_ERR_PLUGIN);

        return;
    }

    (void)flight->api->job_done(flight->job, async_invert(flight->job, flight->api));
}

static void HDRG_PLUGIN_CALL async_cancel(void *inst, const hdrg_plugin_api_t *api)
{
    tp_async_t *async = (tp_async_t *)inst;

    (void)api;

    if (!async->waiting) {
        return;
    }

    hdrg_mutex_lock(&async->lock);
    async->canceled = true;
    async->cancels++;
    hdrg_cond_broadcast(&async->wake);
    hdrg_mutex_unlock(&async->lock);
}

static hdrg_status_t HDRG_PLUGIN_CALL async_create(hdrg_filter_t *filter,
                                                   const hdrg_options_t *options, void **inst,
                                                   const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    *inst = &g_async;
    g_async.filter = filter;
    g_async.inflight[0] = api->filter_set_max_inflight(filter, 0);
    g_async.inflight[1] = api->filter_set_max_inflight(filter, 9);

    status = api->option_get_int(options, TP_LIT("mode"), &g_async.mode);
    status = (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("depth"), &g_async.depth)
                                 : status;

    if (status == HDRG_OK && g_async.mode == 4 && !g_async.waiting) {
        g_async.waiting = hdrg_mutex_init(&g_async.lock) && hdrg_cond_init(&g_async.wake);
        status = g_async.waiting ? HDRG_OK : HDRG_ERR_NOMEM;
    }

    if (status == HDRG_OK && g_async.depth > 0) {
        status = api->filter_set_max_inflight(filter, (uint32_t)g_async.depth);
    }

    return status;
}

static void HDRG_PLUGIN_CALL async_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    tp_async_t *async = (tp_async_t *)inst;
    uint32_t i;

    (void)api;

    for (i = 0; i < 8u; i++) {
        if (async->flights[i].running) {
            hdrg_thread_join(&async->flights[i].thread);
            async->flights[i].running = false;
        }
    }

    if (async->waiting) {
        hdrg_cond_destroy(&async->wake);
        hdrg_mutex_destroy(&async->lock);
        async->waiting = false;
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL async_configure(void *inst, hdrg_negotiation_t *nego,
                                                      const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;
    uint32_t width, height, format;

    (void)inst;

    g_async.inflight[2] = api->filter_set_max_inflight(g_async.filter, 2);

    status = api->nego_input(nego, &width, &height, &format);

    /* the same every frame: written once (jobs in flight read it) */
    if (status == HDRG_OK && g_async.width != width) {
        g_async.width = width;
    }

    return (status == HDRG_OK) ? api->nego_set_output(nego, width, height, format) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL async_submit(void *inst, hdrg_job_t *job,
                                                   const hdrg_plugin_api_t *api)
{
    tp_async_t *async = (tp_async_t *)inst;
    tp_flight_t *flight;
    hdrg_status_t status;
    uint32_t begin, end;
    size_t stride;

    if (async->submits < TP_JOBS &&
        (api->job_input(job, 0, &async->ins[async->submits], &stride, &begin, &end) != HDRG_OK ||
         api->job_output(job, 0, &async->outs[async->submits], &stride) != HDRG_OK)) {
        return HDRG_ERR_PLUGIN;
    }

    flight = &async->flights[async->submits % 8u];
    async->submits++;

    if (flight->running) {
        hdrg_thread_join(&flight->thread);
        flight->running = false;
    }

    switch (async->mode) {
    case 1:
    case 4:
        flight->job = job;
        flight->api = api;
        flight->running = hdrg_thread_create(&flight->thread, async_thread, flight);

        return flight->running ? HDRG_OK : HDRG_ERR_PLUGIN;
    case 2:
        return api->job_done(job, HDRG_ERR_PLUGIN);
    case 3:
        return HDRG_ERR_PLUGIN;
    default:
        status = api->job_done(job, async_invert(job, api));
        async->again = api->job_done(job, HDRG_OK);

        return status;
    }
}

/* the same frames as the asynchronous filter's, from process */
static hdrg_status_t HDRG_PLUGIN_CALL invert_process(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    (void)inst;

    g_async.not_async = api->job_done(job, HDRG_OK);

    return async_invert(job, api);
}

static hdrg_status_t async_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                 const char *id, uint32_t flags, bool with_submit)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, id, strlen(id));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER | HDRG_STAGE_SCALE);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, flags) : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("mode"), 0, 0, 4, NULL, 0)
                                 : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("depth"), 1, 0, 8, NULL, 0)
                                 : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, async_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, async_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, async_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, invert_process) : status;
    status = (status == HDRG_OK && with_submit) ? api->class_set_submit(cls, async_submit) : status;
    status = (status == HDRG_OK && with_submit) ? api->class_set_cancel(cls, async_cancel) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL async_init(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.async"), TP_LIT("Asynchronous"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? async_class(plugin, api, "org.test.async.gpu",
                                               HDRG_FILTER_ASYNC | HDRG_FILTER_DETERMINISTIC, true)
                                 : status;

    return (status == HDRG_OK)
               ? async_class(plugin, api, "org.test.async.cpu", HDRG_FILTER_DETERMINISTIC, false)
               : status;
}

static hdrg_status_t window_make(hdrg_filter_t *filter, const hdrg_options_t *options, void **inst,
                                 const hdrg_plugin_api_t *api, bool record)
{
    tp_window_t *window;
    hdrg_status_t status;

    window = (tp_window_t *)calloc(1, sizeof(*window));
    if (window == NULL) {
        return HDRG_ERR_NOMEM;
    }

    window->filter = filter;
    window->record = record;
    g_windows.bad = api->filter_set_window(filter, 33, 0);

    status = api->option_get_int(options, TP_LIT("past"), &window->past);
    status = (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("future"), &window->future)
                                 : status;
    status =
        (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("clip"), &window->clip) : status;
    status = (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("overlap"), &window->overlap)
                                 : status;

    if (status == HDRG_OK) {
        status =
            (window->clip > 0)
                ? api->filter_set_clip(filter, (uint32_t)window->clip, (uint32_t)window->overlap)
                : api->filter_set_window(filter, (uint32_t)window->past, (uint32_t)window->future);
    }

    if (status != HDRG_OK) {
        free(window);

        return status;
    }

    *inst = window;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL window_create_frame(hdrg_filter_t *filter,
                                                          const hdrg_options_t *options,
                                                          void **inst, const hdrg_plugin_api_t *api)
{
    return window_make(filter, options, inst, api, true);
}

static hdrg_status_t HDRG_PLUGIN_CALL window_create_bands(hdrg_filter_t *filter,
                                                          const hdrg_options_t *options,
                                                          void **inst, const hdrg_plugin_api_t *api)
{
    return window_make(filter, options, inst, api, false);
}

static void HDRG_PLUGIN_CALL window_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)api;

    free(inst);
}

static hdrg_status_t HDRG_PLUGIN_CALL window_configure(void *inst, hdrg_negotiation_t *nego,
                                                       const hdrg_plugin_api_t *api)
{
    tp_window_t *window = (tp_window_t *)inst;
    hdrg_status_t status;
    uint32_t width, height, format, version;

    if (window->record) {
        g_windows.late = api->filter_set_window(window->filter, 1, 1);
        /* the side data's size alone (no buffer) */
        g_windows.side_query =
            api->nego_side_data(nego, HDRG_SD_NOISE, &version, NULL, 0, &g_windows.side_needed);
    }

    status = api->nego_input(nego, &width, &height, &format);
    window->width = width;

    return (status == HDRG_OK) ? api->nego_set_output(nego, width, height, format) : status;
}

/* each sample the rounded mean of the frames of the job's window */
static hdrg_status_t HDRG_PLUGIN_CALL window_process(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const tp_window_t *window = (const tp_window_t *)inst;
    const uint8_t *in[2u * 32u + 1u], *beyond;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, index, past, future, shot_past, shot_future, y, x, j, n,
        sum;
    uint16_t sample;
    uint8_t *out, *spare;
    size_t stride, out_stride;
    void *handle;

    status = api->job_rows(job, &begin, &end);
    status = (status == HDRG_OK) ? api->job_window(job, &index, &past, &future) : status;
    status = (status == HDRG_OK) ? api->job_output(job, 0, &out, &out_stride) : status;
    n = (status == HDRG_OK) ? past + future + 1u : 0u;

    for (j = 0; status == HDRG_OK && j < n; j++) {
        status = api->job_input_frame(job, (int32_t)j - (int32_t)past, 0, &in[j], &stride,
                                      &in_begin, &in_end);
    }

    if (status == HDRG_OK && window->record && g_windows.jobs < sizeof(g_windows.seen)) {
        g_windows.shot = api->job_shot(job, &shot_past, &shot_future);
        g_windows.shots[g_windows.jobs] =
            (g_windows.shot == HDRG_OK) ? (uint8_t)(shot_past << 4 | shot_future) : 0xffu;
        g_windows.index[g_windows.jobs] = index;
        (void)api->job_frame(job, &g_windows.pts[g_windows.jobs], &g_windows.flags[g_windows.jobs]);
        g_windows.seen[g_windows.jobs++] = (uint8_t)(past << 4 | future);
        g_windows.beyond =
            api->job_input_frame(job, (int32_t)future + 1, 0, &beyond, &stride, &in_begin, &in_end);
        /* a plane past the frame's, a frame past the window: refused, nothing read */
        g_windows.plane_in = api->job_input(job, 3, &beyond, &stride, &in_begin, &in_end);
        g_windows.plane_out = api->job_output(job, 3, &spare, &stride);
        g_windows.plane_frame =
            api->job_input_frame(job, 0, 3, &beyond, &stride, &in_begin, &in_end);
        g_windows.handle_beyond = api->job_input_handle(job, (int32_t)future + 1, &handle);
        g_windows.device_beyond = api->job_device_window(job, -(int32_t)past - 1, &handle);
    }

    for (y = begin; status == HDRG_OK && y < end; y++) {
        for (x = 0; x < window->width * 3u; x++) {
            sum = 0;

            for (j = 0; j < n; j++) {
                memcpy(&sample, in[j] + (size_t)(y - in_begin) * stride + x * sizeof(uint16_t),
                       sizeof(sample));
                sum += sample;
            }

            sample = (uint16_t)((sum + n / 2u) / n);
            memcpy(out + (size_t)(y - begin) * out_stride + x * sizeof(uint16_t), &sample,
                   sizeof(sample));
        }
    }

    return status;
}

/* the SEQUENTIAL class: each sample plus 1000 times the jobs since the clip's first */
static hdrg_status_t HDRG_PLUGIN_CALL window_sequence(void *inst, hdrg_job_t *job,
                                                      const hdrg_plugin_api_t *api)
{
    const uint8_t *in;
    tp_window_t *window = (tp_window_t *)inst;
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, flags, y, x;
    uint16_t sample;
    uint8_t *out;
    int64_t pts;
    size_t stride, out_stride;

    flags = 0;

    status = api->job_rows(job, &begin, &end);
    status = (status == HDRG_OK) ? api->job_frame(job, &pts, &flags) : status;
    status =
        (status == HDRG_OK) ? api->job_input(job, 0, &in, &stride, &in_begin, &in_end) : status;
    status = (status == HDRG_OK) ? api->job_output(job, 0, &out, &out_stride) : status;
    window->run = ((flags & HDRG_JOB_FIRST) != 0) ? 0u : window->run + 1u;

    for (y = begin; status == HDRG_OK && y < end; y++) {
        for (x = 0; x < window->width * 3u; x++) {
            memcpy(&sample, in + (size_t)(y - in_begin) * stride + x * sizeof(uint16_t),
                   sizeof(sample));
            sample = (uint16_t)(sample + window->run * 1000u);
            memcpy(out + (size_t)(y - begin) * out_stride + x * sizeof(uint16_t), &sample,
                   sizeof(sample));
        }
    }

    return status;
}

static hdrg_status_t window_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                  const char *id, uint32_t stages, uint32_t flags,
                                  hdrg_filter_create_fn_t create)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, id, strlen(id));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, stages);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, flags) : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("past"), 0, 0, 32, NULL, 0)
                                 : status;
    status = (status == HDRG_OK)
                 ? api->class_add_option_int(cls, TP_LIT("future"), 0, 0, 32, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("clip"), 0, 0, 32, NULL, 0)
                                 : status;
    status = (status == HDRG_OK)
                 ? api->class_add_option_int(cls, TP_LIT("overlap"), 0, 0, 8, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, window_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, window_configure) : status;
    status =
        (status == HDRG_OK)
            ? api->class_set_process(cls, ((flags & HDRG_FILTER_SEQUENTIAL) != 0) ? window_sequence
                                                                                  : window_process)
            : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL window_init(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.window"), TP_LIT("Time window"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? window_class(plugin, api, "org.test.window.frame",
                                                HDRG_STAGE_PREFILTER | HDRG_STAGE_SCALE,
                                                HDRG_FILTER_DETERMINISTIC, window_create_frame)
                                 : status;
    status = (status == HDRG_OK) ? window_class(plugin, api, "org.test.window.seq",
                                                HDRG_STAGE_PREFILTER | HDRG_STAGE_SCALE,
                                                HDRG_FILTER_DETERMINISTIC | HDRG_FILTER_SEQUENTIAL,
                                                window_create_bands)
                                 : status;

    return (status == HDRG_OK)
               ? window_class(plugin, api, "org.test.window.bands",
                              HDRG_STAGE_PREFILTER | HDRG_STAGE_SCALE,
                              HDRG_FILTER_PURE | HDRG_FILTER_BANDS | HDRG_FILTER_DETERMINISTIC,
                              window_create_bands)
               : status;
}

/* the bytes of a row of the format's plane at width */
static size_t format_row(uint32_t format, uint32_t plane, uint32_t width)
{
    switch (format) {
    case HDRG_FORMAT_RGB16:
        return (size_t)width * 6u;
    case HDRG_FORMAT_RGBA16:
        return (size_t)width * 8u;
    case HDRG_FORMAT_P016:
        return (plane == 0) ? (size_t)width * 2u : ((size_t)width + 1u) / 2u * 4u;
    case HDRG_FORMAT_RGBA8:
    case HDRG_FORMAT_RGB10A2:
    case HDRG_FORMAT_RGBF32_PLANAR:
        return (size_t)width * 4u;
    default:
        return (size_t)width * 2u;
    }
}

static hdrg_status_t HDRG_PLUGIN_CALL format_create(hdrg_filter_t *filter,
                                                    const hdrg_options_t *options, void **inst,
                                                    const hdrg_plugin_api_t *api)
{
    tp_format_t *format;
    hdrg_status_t status;
    int64_t matrix, full;

    format = (tp_format_t *)calloc(1, sizeof(*format));
    if (format == NULL) {
        return HDRG_ERR_NOMEM;
    }

    status = api->option_get_int(options, TP_LIT("in"), &format->in);
    status =
        (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("out"), &format->out) : status;
    status =
        (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("past"), &format->past) : status;
    status = (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("matrix"), &matrix) : status;
    status = (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("full"), &full) : status;
    status =
        (status == HDRG_OK) ? api->option_get_int(options, TP_LIT("fail"), &format->fail) : status;
    status = (status == HDRG_OK && format->fail == 6) ? HDRG_ERR_UNSUPPORTED : status;
    status = (status == HDRG_OK && format->fail == 7) ? HDRG_ERR_INTERNAL : status;
    status = (status == HDRG_OK) ? api->filter_set_input(filter, (uint32_t)format->in,
                                                         (uint32_t)matrix, (uint8_t)full)
                                 : status;
    status = (status == HDRG_OK && format->past > 0)
                 ? api->filter_set_window(filter, (uint32_t)format->past, 0)
                 : status;
    if (status != HDRG_OK) {
        free(format);

        return status;
    }

    *inst = format;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL format_configure(void *inst, hdrg_negotiation_t *nego,
                                                       const hdrg_plugin_api_t *api)
{
    tp_format_t *format = (tp_format_t *)inst;
    hdrg_status_t status;
    uint32_t seen;

    status = api->nego_input(nego, &format->width, &format->height, &seen);
    if (status == HDRG_OK && seen != (uint32_t)format->in) {
        status = HDRG_ERR_PLUGIN;
    }

    return (status == HDRG_OK)
               ? api->nego_set_output(nego, format->width, format->height, (uint32_t)format->out)
               : status;
}

/*
 * a frame of its own: strides 64 bytes past a packed row (fail 2: 2 bytes
 * short of it; 4: 65 past; 5: the planes one byte off the block's start)
 */
static hdrg_status_t HDRG_PLUGIN_CALL format_alloc(void *inst, uint32_t format, uint32_t width,
                                                   uint32_t height, uint8_t **planes,
                                                   size_t *strides, void **handle,
                                                   const hdrg_plugin_api_t *api)
{
    const tp_format_t *own = (const tp_format_t *)inst;
    hdrg_format_layout_t layout;
    tp_frame_t *frame;
    uint8_t p;
    size_t bytes, rows[3];

    (void)api;

    if (own->fail == 1 || !hdrg_format_layout(format, width, height, false, &layout)) {
        return HDRG_ERR_NOMEM;
    }

    frame = (tp_frame_t *)calloc(1, sizeof(*frame));
    if (frame == NULL) {
        return HDRG_ERR_NOMEM;
    }

    bytes = 0;

    for (p = 0; p < layout.planes; p++) {
        frame->strides[p] = (own->fail == 2)   ? layout.strides[p] - 2u
                            : (own->fail == 4) ? layout.strides[p] + 65u
                                               : layout.strides[p] + 64u;
        rows[p] = (format == HDRG_FORMAT_P016 && p == 1u) ? (height + 1u) / 2u : height;
        bytes += frame->strides[p] * rows[p];
    }

    frame->block = malloc(bytes + 1u);
    if (frame->block == NULL) {
        free(frame);

        return HDRG_ERR_NOMEM;
    }

    bytes = (own->fail == 5) ? 1u : 0u;

    for (p = 0; p < layout.planes; p++) {
        frame->planes[p] = (uint8_t *)frame->block + bytes;
        planes[p] = frame->planes[p];
        strides[p] = frame->strides[p];
        bytes += frame->strides[p] * rows[p];
    }

    *handle = frame;
    g_frames.made++;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL format_free(void *inst, void *handle, const hdrg_plugin_api_t *api)
{
    tp_frame_t *frame = (tp_frame_t *)handle;

    (void)inst;
    (void)api;

    free(frame->block);
    free(frame);
    g_frames.freed++;
}

/* the job's frames are the plugin's own (the input, its window's past, the output) */
static void format_check(const hdrg_job_t *job, uint32_t past, const hdrg_plugin_api_t *api)
{
    const tp_frame_t *own;
    const uint8_t *in;
    hdrg_status_t status;
    uint32_t begin, end, j;
    uint8_t *out;
    size_t stride;
    void *handle;

    for (j = 0; j <= past; j++) {
        status = api->job_input_handle(job, -(int32_t)j, &handle);
        status = (status == HDRG_OK)
                     ? api->job_input_frame(job, -(int32_t)j, 0, &in, &stride, &begin, &end)
                     : status;
        own = (const tp_frame_t *)handle;

        if (status != HDRG_OK || own == NULL || in != own->planes[0] || stride != own->strides[0]) {
            g_frames.mismatched++;
        }

        g_frames.windows += (j > 0) ? 1u : 0u;
    }

    status = api->job_output_handle(job, &handle);
    status = (status == HDRG_OK) ? api->job_output(job, 0, &out, &stride) : status;
    own = (const tp_frame_t *)handle;

    if (status != HDRG_OK || own == NULL || out != own->planes[0] || stride != own->strides[0]) {
        g_frames.mismatched++;
    }
}

/*
 * each plane's rows copied, or (past > 0, 16-bit samples) the rounded mean
 * of the window's frames; the input and output formats are one
 */
static hdrg_status_t HDRG_PLUGIN_CALL format_process(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const tp_format_t *format = (const tp_format_t *)inst;
    const uint8_t *in[9];
    hdrg_status_t status;
    uint32_t begin, end, in_begin, in_end, index, past, future, plane, planes, y, rows, x, j, sum;
    uint16_t sample;
    uint8_t *out;
    size_t stride, out_stride, row;

    planes = (format->in == HDRG_FORMAT_RGB16_PLANAR || format->in == HDRG_FORMAT_RGBF16_PLANAR ||
              format->in == HDRG_FORMAT_RGBF32_PLANAR)
                 ? 3u
                 : ((format->in == HDRG_FORMAT_P016) ? 2u : 1u);

    status = (format->in == format->out) ? api->job_rows(job, &begin, &end) : HDRG_ERR_PLUGIN;
    status = (status == HDRG_OK) ? api->job_window(job, &index, &past, &future) : status;

    if (status == HDRG_OK && format->fail == 3) {
        format_check(job, past, api);
    }

    for (plane = 0; status == HDRG_OK && plane < planes; plane++) {
        status = api->job_output(job, plane, &out, &out_stride);

        for (j = 0; status == HDRG_OK && j <= past; j++) {
            status =
                api->job_input_frame(job, -(int32_t)j, plane, &in[j], &stride, &in_begin, &in_end);
        }

        row = format_row((uint32_t)format->in, plane, format->width);
        rows =
            (format->in == HDRG_FORMAT_P016 && plane == 1u) ? (end - begin + 1u) / 2u : end - begin;

        for (y = 0; status == HDRG_OK && y < rows; y++) {
            if (past == 0) {
                memcpy(out + (size_t)y * out_stride,
                       in[0] + (size_t)(y + begin - in_begin) * stride, row);

                continue;
            }

            for (x = 0; x < row / sizeof(uint16_t); x++) {
                sum = 0;

                for (j = 0; j <= past; j++) {
                    memcpy(&sample,
                           in[j] + (size_t)(y + begin - in_begin) * stride + x * sizeof(uint16_t),
                           sizeof(sample));
                    sum += sample;
                }

                sample = (uint16_t)((sum + (past + 1u) / 2u) / (past + 1u));
                memcpy(out + (size_t)y * out_stride + x * sizeof(uint16_t), &sample,
                       sizeof(sample));
            }
        }
    }

    return status;
}

static hdrg_status_t format_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                  const char *id, uint32_t flags, bool own)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, id, strlen(id));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER | HDRG_STAGE_SCALE);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, flags) : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("in"), 1, 1, 8, NULL, 0)
                                 : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("out"), 1, 1, 8, NULL, 0)
                                 : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("past"), 0, 0, 8, NULL, 0)
                                 : status;
    status = (status == HDRG_OK)
                 ? api->class_add_option_int(cls, TP_LIT("matrix"), 1, 1, 9, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("full"), 0, 0, 1, NULL, 0)
                                 : status;
    status = (status == HDRG_OK) ? api->class_add_option_int(cls, TP_LIT("fail"), 0, 0, 7, NULL, 0)
                                 : status;
    status =
        (status == HDRG_OK && own) ? api->class_set_frames(cls, format_alloc, format_free) : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, format_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, window_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, format_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, format_process) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL format_init(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.format"), TP_LIT("Formats"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? format_class(plugin, api, "org.test.format.frame",
                                                HDRG_FILTER_DETERMINISTIC, false)
                                 : status;
    status = (status == HDRG_OK)
                 ? format_class(plugin, api, "org.test.format.own", HDRG_FILTER_DETERMINISTIC, true)
                 : status;

    return (status == HDRG_OK)
               ? format_class(plugin, api, "org.test.format.bands",
                              HDRG_FILTER_PURE | HDRG_FILTER_BANDS | HDRG_FILTER_DETERMINISTIC,
                              false)
               : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL device_create(hdrg_filter_t *filter,
                                                    const hdrg_options_t *options, void **inst,
                                                    const hdrg_plugin_api_t *api)
{
    (void)options;

    *inst = &g_devices;
    (void)api->filter_device(filter, HDRG_DEVICE_CUDA, &g_devices.device, &g_devices.queue);

    return api->filter_set_device(filter, HDRG_DEVICE_CUDA);
}

static void HDRG_PLUGIN_CALL device_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;
}

static hdrg_status_t HDRG_PLUGIN_CALL device_configure(void *inst, hdrg_negotiation_t *nego,
                                                       const hdrg_plugin_api_t *api)
{
    tp_devices_t *devices = (tp_devices_t *)inst;
    hdrg_status_t status;
    uint32_t height, format;

    status = api->nego_input(nego, &devices->width, &height, &format);

    return (status == HDRG_OK) ? api->nego_set_output(nego, devices->width, height, format)
                               : status;
}

/* the frame inverted: from and to the host's planes or frames of its own */
static hdrg_status_t HDRG_PLUGIN_CALL device_process(void *inst, hdrg_job_t *job,
                                                     const hdrg_plugin_api_t *api)
{
    const uint8_t *in;
    tp_devices_t *devices = (tp_devices_t *)inst;
    hdrg_status_t status;
    uint64_t ready;
    uint32_t begin, end, in_begin, in_end, y, x;
    uint16_t sample, *frame;
    uint8_t wanted, *out;
    size_t in_stride, out_stride, row;
    void *device;

    row = (size_t)devices->width * 3u * sizeof(uint16_t);
    frame = NULL;

    status = api->job_rows(job, &begin, &end);
    status = (status == HDRG_OK) ? api->job_device(job, &device, &ready, &wanted) : status;
    if (status != HDRG_OK) {
        return status;
    }

    if (device != NULL) {
        in = (const uint8_t *)device;
        in_stride = row;
        devices->device_in++;
    } else {
        status = api->job_input(job, 0, &in, &in_stride, &in_begin, &in_end);
        devices->host_in++;
    }

    if (status == HDRG_OK && wanted != 0) {
        frame = (uint16_t *)malloc(row * (end - begin));
        status =
            (frame != NULL) ? api->job_set_output_device(job, frame, ready + 1u) : HDRG_ERR_NOMEM;
        out = (uint8_t *)frame;
        out_stride = row;
        devices->device_out++;
    } else if (status == HDRG_OK) {
        status = api->job_output(job, 0, &out, &out_stride);
        devices->host_out++;
    }

    for (y = 0; status == HDRG_OK && y < end - begin; y++) {
        for (x = 0; x < devices->width * 3u; x++) {
            memcpy(&sample, in + (size_t)y * in_stride + x * sizeof(uint16_t), sizeof(sample));
            sample = (uint16_t)(65535u - sample);
            memcpy(out + (size_t)y * out_stride + x * sizeof(uint16_t), &sample, sizeof(sample));
        }
    }

    return status;
}

static void HDRG_PLUGIN_CALL device_release(void *inst, void *frame, const hdrg_plugin_api_t *api)
{
    (void)api;

    free(frame);
    ((tp_devices_t *)inst)->released++;
}

static hdrg_status_t device_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                  const char *id, bool releases)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    cls = api->class_create(plugin, id, strlen(id));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_PREFILTER);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, HDRG_FILTER_DETERMINISTIC) : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, device_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, device_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, device_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, device_process) : status;
    status = (status == HDRG_OK && releases) ? api->class_set_device_release(cls, device_release)
                                             : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL device_init(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.device"), TP_LIT("Device"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    status =
        (status == HDRG_OK) ? device_class(plugin, api, "org.test.device.invert", true) : status;

    return (status == HDRG_OK) ? device_class(plugin, api, "org.test.device.norelease", false)
                               : status;
}

static hdrg_status_t valid_info(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, g_id, strlen(g_id), TP_LIT("Good"), TP_LIT("1.0.0"),
                                  TP_LIT("Tests"));
    if (status == HDRG_OK) {
        status = api->plugin_set_license(plugin, g_license, strlen(g_license),
                                         TP_LIT("Copyright (c) 2026 Tests"));
    }

    if (status == HDRG_OK) {
        status = api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0);
    }

    return status;
}

/* a complete class with options; register = 0 leaves it unregistered */
static hdrg_class_t *valid_class(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api,
                                 bool register_it, bool with_process)
{
    hdrg_class_t *cls;
    hdrg_status_t status;
    char id[160];

    snprintf(id, sizeof(id), "%s.scale", g_id);

    cls = api->class_create(plugin, id, strlen(id));
    if (cls == NULL) {
        return NULL;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    status = (status == HDRG_OK) ? api->class_set_flags(cls, HDRG_FILTER_BANDS) : status;
    status = (status == HDRG_OK)
                 ? api->class_add_option_int(cls, TP_LIT("taps"), 3, 2, 8, TP_LIT("kernel lobes"))
                 : status;
    status = (status == HDRG_OK)
                 ? api->class_add_option_float(cls, TP_LIT("sharp"), 0.5, 0.0, 1.0, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? api->class_set_create(cls, stub_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, stub_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, stub_configure) : status;
    status =
        (status == HDRG_OK && with_process) ? api->class_set_process(cls, stub_process) : status;
    g_api_status = status;

    if (status == HDRG_OK && register_it) {
        g_api_status = api->class_register(cls);
    }

    return cls;
}

static hdrg_status_t HDRG_PLUGIN_CALL good_init(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    g_saved_plugin = plugin;
    g_saved_api = api;

    status = valid_info(plugin, api);
    if (status != HDRG_OK) {
        return status;
    }

    g_saved_class = valid_class(plugin, api, true, true);

    return (g_saved_class != NULL) ? g_api_status : HDRG_ERR_PLUGIN;
}

static uint32_t g_deinits;

static void HDRG_PLUGIN_CALL counting_deinit(void *user, const hdrg_plugin_api_t *api)
{
    (void)user;
    (void)api;

    g_deinits++;
}

/* a plugin with a filter that says it cannot work here (no device for it) */
static hdrg_status_t HDRG_PLUGIN_CALL unavailable_init(hdrg_plugin_t *plugin,
                                                       const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;

    status = valid_info(plugin, api);
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_UNAVAILABLE, HDRG_REASON_NO_DEVICE,
                                          TP_LIT("needs a device"))
                 : status;
    status = (status == HDRG_OK) ? api->plugin_set_deinit(plugin, counting_deinit) : status;

    if (status != HDRG_OK) {
        return status;
    }

    return (valid_class(plugin, api, true, true) != NULL) ? g_api_status : HDRG_ERR_PLUGIN;
}

/*
 * an app's STATS plugin: it counts its calls, reads the queue, renders each frame with half its
 * anchor and keeps 2 frames ahead (its window); bad: one with a process (refused)
 */
typedef struct {
    uint32_t fades, frames, renders, fits, rendered, queued, last_index, faults;
    bool session_seen;
} tp_stats_t;

static tp_stats_t g_stats;
static bool g_stats_bad;

static hdrg_status_t HDRG_PLUGIN_CALL stats_create(hdrg_filter_t *filter,
                                                   const hdrg_options_t *options, void **inst,
                                                   const hdrg_plugin_api_t *api)
{
    const hdrg_stats_session_t *session;

    (void)options;

    g_stats.session_seen = api->filter_stats_session(filter, &session) == HDRG_OK &&
                           session->temporal == HDRG_TEMPORAL_RECURSIVE && session->width == TP_W &&
                           session->height == TP_H && session->scene_reach > 0u &&
                           session->rate > 0.0f;
    *inst = &g_stats;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL stats_window(void *inst, uint32_t latency, uint32_t *past,
                                          uint32_t *future, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;

    *past = 3;
    *future = (latency == HDRG_LATENCY_LOW) ? 0u : 2u;
}

static hdrg_status_t HDRG_PLUGIN_CALL stats_fade(void *inst, const hdrg_stats_sent_t *sent,
                                                 hdrg_stats_fade_t *out,
                                                 const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;

    g_stats.fades++;
    g_stats.faults += (out->run == 0 && out->from == sent->index && out->dim == 1.0f) ? 0u : 1u;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL stats_frame(void *inst, const hdrg_stats_frame_t *frame,
                                                  const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;

    g_stats.frames++;
    g_stats.faults += (frame->index + 1u == g_stats.frames) ? 0u : 1u;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL stats_render(void *inst, hdrg_rendering_t *rendering,
                                                   const hdrg_plugin_api_t *api)
{
    const hdrg_stats_frame_t *queued;
    hdrg_stats_head_t *head;
    hdrg_status_t status;
    uint32_t k;

    (void)inst;

    status = api->stats_head(rendering, &head);
    if (status != HDRG_OK) {
        return status;
    }

    /* the queue: the frames sent after it, in order */
    for (k = 1; api->stats_queued(rendering, k, &queued) == HDRG_OK; k++) {
        g_stats.queued++;
        g_stats.faults += (queued->index == head->index + k) ? 0u : 1u;
    }

    g_stats.renders++;
    g_stats.last_index = head->index;
    head->anchor *= 0.5f;

    return HDRG_OK;
}

static void HDRG_PLUGIN_CALL stats_fit(void *inst, uint32_t index, float fit, uint8_t *replace,
                                       float *used, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)index;
    (void)fit;
    (void)replace;
    (void)used;
    (void)api;

    g_stats.fits++;
}

static void HDRG_PLUGIN_CALL stats_rendered(void *inst, uint32_t index, float anchor, float key,
                                            float fit, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)anchor;
    (void)key;
    (void)fit;
    (void)api;

    g_stats.rendered++;
    g_stats.faults += (index == g_stats.last_index) ? 0u : 1u;
}

static hdrg_status_t HDRG_PLUGIN_CALL stats_init(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.stats"), TP_LIT("Stats"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, TP_LIT("org.test.stats.half"));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_STATS);
    status = (status == HDRG_OK) ? api->class_set_create(cls, stats_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, stub_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_stats(cls, stats_window, stats_fade, stats_frame,
                                                        stats_render, stats_fit, stats_rendered)
                                 : status;
    status =
        (status == HDRG_OK && g_stats_bad) ? api->class_set_process(cls, stub_process) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

/*
 * an ANALYZE plugin: every light's chance 0 (its boost gone), its records and frame checked;
 * bad: an ANALYZE class with a process (refused)
 */
static uint32_t g_analyzed, g_analyze_faults;
static bool g_analyze_bad;

static hdrg_status_t HDRG_PLUGIN_CALL analyze_zero(void *inst, hdrg_lights_t *lights,
                                                   const hdrg_plugin_api_t *api)
{
    const hdrg_light_record_t *records;
    const uint16_t *rgb;
    hdrg_status_t status;
    uint32_t count, k, width, height;
    size_t stride;

    (void)inst;

    status = api->lights_get(lights, &records, &count);
    status =
        (status == HDRG_OK) ? api->lights_frame(lights, &rgb, &stride, &width, &height) : status;

    if (status != HDRG_OK || count == 0 || stride < (size_t)width * 6u) {
        g_analyze_faults++;

        return HDRG_ERR_INTERNAL;
    }

    for (k = 0; k < count; k++) {
        if (records[k].width == 0 || records[k].x + records[k].width > width ||
            records[k].height == 0 || records[k].y + records[k].height > height ||
            !(records[k].boost >= 0.0f && records[k].boost <= 1.0f)) {
            g_analyze_faults++;
        }

        status = (status == HDRG_OK) ? api->lights_set_chance(lights, k, 0.0f) : status;
    }

    /* past the lights, or past 0..1: refused */
    if (api->lights_set_chance(lights, count, 0.0f) != HDRG_ERR_INVALID_ARG ||
        api->lights_set_chance(lights, 0, 1.5f) != HDRG_ERR_INVALID_ARG) {
        g_analyze_faults++;
    }

    g_analyzed += count;

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL analyze_init(hdrg_plugin_t *plugin,
                                                   const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.analyze"), TP_LIT("Analyze"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, TP_LIT("org.test.analyze.zero"));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_ANALYZE);
    status = (status == HDRG_OK) ? api->class_set_create(cls, stub_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, stub_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_analyze(cls, analyze_zero) : status;
    status =
        (status == HDRG_OK && g_analyze_bad) ? api->class_set_process(cls, stub_process) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

/*
 * plugins asking for the context's GPU devices (filter_gpu_share) in their filters' create: each
 * kind's share, and ones of another version and a smaller struct (refused); a WebGPU share's run
 * counted
 */
static hdrg_gpu_share_t g_shares[2][HDRG_DEVICE_KINDS];
static hdrg_status_t g_share_other, g_share_small;
static hdrg_atomic_u32_t g_share_ran;
static uint32_t g_share_runs;
static uint8_t g_share_plugin;

static void HDRG_PLUGIN_CALL share_ran(void *arg)
{
    (void)hdrg_atomic_u32_fetch_add_acq_rel((hdrg_atomic_u32_t *)arg, 1u);
}

static hdrg_status_t HDRG_PLUGIN_CALL layer_create(hdrg_filter_t *filter,
                                                   const hdrg_options_t *options, void **inst,
                                                   const hdrg_plugin_api_t *api)
{
    hdrg_gpu_share_t *share, other;
    uint32_t k;

    (void)options;

    for (k = 1; k <= HDRG_DEVICE_KINDS; k++) {
        share = &g_shares[g_share_plugin][k - 1u];
        memset(share, 0, sizeof(*share));
        share->struct_size = (uint32_t)sizeof(*share);
        share->version = HDRG_GPU_SHARE_VERSION;

        if (api->filter_gpu_share(filter, k, share) != HDRG_OK) {
            memset(share, 0, sizeof(*share));
        } else if (k == HDRG_DEVICE_WEBGPU &&
                   share->run(share->user, share_ran, &g_share_ran) == HDRG_OK) {
            g_share_runs++;
        }
    }

    memset(&other, 0, sizeof(other));
    other.struct_size = (uint32_t)sizeof(other);
    other.version = HDRG_GPU_SHARE_VERSION + 1u;
    g_share_other = api->filter_gpu_share(filter, HDRG_DEVICE_WEBGPU, &other);
    g_share_other = (other.device == NULL) ? g_share_other : HDRG_ERR_INTERNAL;
    other.struct_size = (uint32_t)sizeof(other) - 1u;
    other.version = HDRG_GPU_SHARE_VERSION;
    g_share_small = api->filter_gpu_share(filter, HDRG_DEVICE_WEBGPU, &other);
    *inst = NULL;

    return HDRG_ERR_UNSUPPORTED;
}

static hdrg_status_t layer_init(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api, const char *id)
{
    hdrg_class_t *cls;
    hdrg_status_t status;
    char name[64];

    status = api->plugin_set_info(plugin, id, strlen(id), TP_LIT("Layer"), TP_LIT("1.0.0"),
                                  TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;

    if (status != HDRG_OK) {
        return status;
    }

    snprintf(name, sizeof(name), "%s.scale", id);

    cls = api->class_create(plugin, name, strlen(name));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_stages(cls, HDRG_STAGE_SCALE);
    status = (status == HDRG_OK) ? api->class_set_create(cls, layer_create) : status;
    status = (status == HDRG_OK) ? api->class_set_destroy(cls, stub_destroy) : status;
    status = (status == HDRG_OK) ? api->class_set_configure(cls, stub_configure) : status;
    status = (status == HDRG_OK) ? api->class_set_process(cls, stub_process) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

static hdrg_status_t HDRG_PLUGIN_CALL layer_init_a(hdrg_plugin_t *plugin,
                                                   const hdrg_plugin_api_t *api)
{
    return layer_init(plugin, api, "org.test.layer.a");
}

static hdrg_status_t HDRG_PLUGIN_CALL layer_init_b(hdrg_plugin_t *plugin,
                                                   const hdrg_plugin_api_t *api)
{
    return layer_init(plugin, api, "org.test.layer.b");
}

/* a codec plugin reading its pointers back: the plugin's and the class's */
static uint8_t g_codec_plugin_user, g_codec_class_user;
static uint32_t g_codec_seen;

static bool codec_users_match(void *plugin_user, void *class_user)
{
    return plugin_user == &g_codec_plugin_user && class_user == &g_codec_class_user;
}

static uint32_t HDRG_PLUGIN_CALL codec_probe(const uint8_t *data, size_t size,
                                             const hdrg_plugin_api_t *api)
{
    (void)api;

    return (size >= 8 && memcmp(data, "TPCODEC1", 8) == 0) ? 1u : 0u;
}

static hdrg_status_t HDRG_PLUGIN_CALL codec_read(hdrg_reader_t *reader, const uint8_t *data,
                                                 size_t size, const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;
    uint8_t *pixels;
    size_t stride;
    void *plugin_user, *class_user;

    (void)data;
    (void)size;

    status = api->read_users(reader, &plugin_user, &class_user);
    if (status == HDRG_OK && codec_users_match(plugin_user, class_user)) {
        g_codec_seen++;
    }

    status = (status == HDRG_OK) ? api->read_image(reader, 4, 4, 8, &pixels, &stride) : status;
    if (status == HDRG_OK) {
        memset(pixels, 128, stride * 4u);
    }

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL codec_create(hdrg_writer_t *writer,
                                                   const hdrg_options_t *options, void **inst,
                                                   const hdrg_plugin_api_t *api)
{
    hdrg_status_t status;
    void *plugin_user, *class_user;

    (void)options;

    status = api->write_users(writer, &plugin_user, &class_user);
    if (status == HDRG_OK && codec_users_match(plugin_user, class_user)) {
        g_codec_seen++;
    }

    *inst = &g_codec_seen;

    return status;
}

static hdrg_status_t HDRG_PLUGIN_CALL codec_rows(void *inst, uint32_t y0, uint32_t rows,
                                                 const uint8_t *data, size_t stride,
                                                 const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)y0;
    (void)rows;
    (void)data;
    (void)stride;
    (void)api;

    return HDRG_OK;
}

static hdrg_status_t HDRG_PLUGIN_CALL codec_finish(void *inst, hdrg_writer_t *writer,
                                                   const hdrg_plugin_api_t *api)
{
    (void)inst;

    return api->write_output(writer, (const uint8_t *)"TPOUT", 5);
}

static void HDRG_PLUGIN_CALL codec_destroy(void *inst, const hdrg_plugin_api_t *api)
{
    (void)inst;
    (void)api;
}

static hdrg_status_t HDRG_PLUGIN_CALL codec_init(hdrg_plugin_t *plugin,
                                                 const hdrg_plugin_api_t *api)
{
    hdrg_class_t *cls;
    hdrg_status_t status;

    status = api->plugin_set_info(plugin, TP_LIT("org.test.codec"), TP_LIT("Codec"),
                                  TP_LIT("1.0.0"), TP_LIT("Tests"));
    status = (status == HDRG_OK) ? api->plugin_set_license(plugin, TP_LIT("MIT"),
                                                           TP_LIT("Copyright (c) 2026 Tests"))
                                 : status;
    status = (status == HDRG_OK)
                 ? api->plugin_set_status(plugin, HDRG_PLUGIN_READY, HDRG_REASON_NONE, NULL, 0)
                 : status;
    status = (status == HDRG_OK) ? api->plugin_set_user(plugin, &g_codec_plugin_user) : status;

    if (status != HDRG_OK) {
        return status;
    }

    cls = api->class_create(plugin, TP_LIT("org.test.codec.io"));
    if (cls == NULL) {
        return HDRG_ERR_PLUGIN;
    }

    status = api->class_set_extensions(cls, TP_LIT("tpc"));
    status = (status == HDRG_OK) ? api->class_set_reader(cls, codec_probe, codec_read) : status;
    status = (status == HDRG_OK)
                 ? api->class_set_writer(cls, HDRG_CODEC_FORM(HDRG_OUTPUT_PQ), HDRG_OUTPUT_PQ,
                                         HDRG_CODEC_ROWS_RGB16, HDRG_CODEC_COLOR_CICP, 65535u,
                                         codec_create, codec_rows, codec_finish, codec_destroy)
                 : status;
    status = (status == HDRG_OK) ? api->class_set_user(cls, &g_codec_class_user) : status;

    return (status == HDRG_OK) ? api->class_register(cls) : status;
}

/* a plugin of the report alone (the same id as good_init's) */
static hdrg_status_t HDRG_PLUGIN_CALL report_init(hdrg_plugin_t *plugin,
                                                  const hdrg_plugin_api_t *api)
{
    return valid_info(plugin, api);
}

/* one deliberately bad report per g_case; g_api_status = what the bad call returned */
static hdrg_status_t HDRG_PLUGIN_CALL bad_init(hdrg_plugin_t *plugin, const hdrg_plugin_api_t *api)
{
    static const size_t name_lens[] = {2, 3, 5, 8, 6, 2, 4, 10};
    static const char *const names[] =
        {
            "\xC0\xAF",        /* overlong */
            "\xED\xA0\x80",    /* surrogate */
            "ab\0cd",          /* NUL inside */
            "\x1B[31mred",     /* terminal escape */
            "abc\xE2\x80\xAE", /* right-to-left override */
            "\xE3\x81",        /* truncated sequence */
            "a\xC2\x85z",      /* C1 control */
            "line\nbreak",     /* no line feeds in a name */
        },
                             *const ids[] = {"Org.Example",  "hdrg.evil",    "example",
                                             "org..example", "org.example.", "1org.example"},
                             *const licenses[] = {"MIT AND",
                                                  "(MIT",
                                                  "MIT and Apache-2.0",
                                                  "((((((((( MIT )))))))))",
                                                  "MIT WITH",
                                                  "GPL-2.0+ +",
                                                  "MIT OR OR BSD-3-Clause",
                                                  "LicenseRef-"};
    hdrg_class_t *cls;
    uint32_t i;

    g_api_status = HDRG_OK;

    if (g_case < 8) {
        g_api_status = api->plugin_set_info(plugin, TP_LIT("org.bad.name"), names[g_case],
                                            name_lens[g_case], TP_LIT("1.0"), NULL, 0);

        return g_api_status;
    }

    if (g_case < 14) {
        g_api_status = api->plugin_set_info(plugin, ids[g_case - 8], strlen(ids[g_case - 8]),
                                            TP_LIT("Bad"), TP_LIT("1.0"), NULL, 0);

        return g_api_status;
    }

    if (valid_info(plugin, api) != HDRG_OK) {
        return HDRG_ERR_INTERNAL;
    }

    switch (g_case) {
    case 14:
        g_api_status = api->plugin_set_info(plugin, TP_LIT("org.bad.version"), TP_LIT("Bad"),
                                            TP_LIT("1.0 beta"), NULL, 0);

        break;
    case 15:
        g_api_status =
            api->plugin_set_info(plugin, TP_LIT("org.bad.long"), g_big, 257, TP_LIT("1"), NULL, 0);

        break;
    case 16:
        g_api_status = api->plugin_set_info(plugin, NULL, 5, TP_LIT("Bad"), TP_LIT("1"), NULL, 0);

        break;
    case 17:
        g_api_status = api->plugin_add_license_text(plugin, TP_LIT("MIT"), TP_LIT("text"));

        break;
    case 18:
        g_api_status =
            api->plugin_add_license_text(plugin, TP_LIT("LicenseRef-Big"), g_big, sizeof(g_big));

        break;
    case 19:
        g_api_status =
            api->plugin_set_status(plugin, HDRG_PLUGIN_DEGRADED, HDRG_REASON_NO_DEVICE, NULL, 0);

        break;
    case 20:
        g_api_status = api->plugin_set_status(plugin, HDRG_PLUGIN_UNAVAILABLE,
                                              HDRG_REASON_HOST_TOO_OLD, NULL, 0);

        break;
    case 21:
        for (i = 0; i <= 64 && g_api_status == HDRG_OK; i++) {
            g_api_status = api->plugin_add_component(plugin, TP_LIT("lib"), TP_LIT("1"),
                                                     TP_LIT("MIT"), TP_LIT("(c) x"), NULL, 0);
        }

        break;
    case 22:
        cls = api->class_create(plugin, TP_LIT("org.test.good.opt"));
        g_api_status = (cls != NULL)
                           ? api->class_add_option_int(cls, TP_LIT("taps"), 10, 0, 5, NULL, 0)
                           : HDRG_ERR_INTERNAL;

        break;
    case 23:
        cls = api->class_create(plugin, TP_LIT("org.test.good.opt"));
        g_api_status = (cls != NULL) ? api->class_add_option_float(cls, TP_LIT("gain"), 0.5, 0.0,
                                                                   HUGE_VAL, NULL, 0)
                                     : HDRG_ERR_INTERNAL;

        break;
    case 24:
        (void)valid_class(plugin, api, true, false);

        break;
    default:
        g_api_status = api->plugin_set_license(plugin, licenses[g_case - 25],
                                               strlen(licenses[g_case - 25]), TP_LIT("(c) x"));

        break;
    }

    return g_api_status;
}

/* SEQUENTIAL with BANDS */
static hdrg_status_t HDRG_PLUGIN_CALL sequence_bad_init(hdrg_plugin_t *plugin,
                                                        const hdrg_plugin_api_t *api)
{
    g_api_status = valid_info(plugin, api);
    if (g_api_status != HDRG_OK) {
        return HDRG_ERR_INTERNAL;
    }

    g_api_status = window_class(plugin, api, "org.test.good.sequence", HDRG_STAGE_SCALE,
                                HDRG_FILTER_SEQUENTIAL | HDRG_FILTER_BANDS, window_create_bands);

    return g_api_status;
}

/* frames of its own with BANDS */
static hdrg_status_t HDRG_PLUGIN_CALL frames_bad_init(hdrg_plugin_t *plugin,
                                                      const hdrg_plugin_api_t *api)
{
    g_api_status = valid_info(plugin, api);
    if (g_api_status != HDRG_OK) {
        return HDRG_ERR_INTERNAL;
    }

    g_api_status = format_class(plugin, api, "org.test.good.frames",
                                HDRG_FILTER_PURE | HDRG_FILTER_BANDS, true);

    return g_api_status;
}

/* g_case 0: an ASYNC class without submit (process does not stand in); 1: ASYNC with BANDS */
static hdrg_status_t HDRG_PLUGIN_CALL async_bad_init(hdrg_plugin_t *plugin,
                                                     const hdrg_plugin_api_t *api)
{
    g_api_status = valid_info(plugin, api);
    if (g_api_status != HDRG_OK) {
        return HDRG_ERR_INTERNAL;
    }

    g_api_status = (g_case == 0)
                       ? async_class(plugin, api, "org.test.good.async", HDRG_FILTER_ASYNC, false)
                       : async_class(plugin, api, "org.test.good.async",
                                     HDRG_FILTER_ASYNC | HDRG_FILTER_BANDS, true);

    return g_api_status;
}

/* complete-looking calls whose report as a whole is invalid */
static hdrg_status_t HDRG_PLUGIN_CALL incomplete_init(hdrg_plugin_t *plugin,
                                                      const hdrg_plugin_api_t *api)
{
    switch (g_case) {
    case 0: /* no status */
        g_api_status = api->plugin_set_info(plugin, TP_LIT("org.bad.nostatus"), TP_LIT("x"),
                                            TP_LIT("1"), NULL, 0);
        g_api_status = (g_api_status == HDRG_OK)
                           ? api->plugin_set_license(plugin, TP_LIT("MIT"), TP_LIT("(c) x"))
                           : g_api_status;

        break;
    case 1: /* LicenseRef- without its text */
        g_license = "MIT OR LicenseRef-Custom";
        g_api_status = valid_info(plugin, api);
        g_license = "MIT";

        break;
    default: /* a class created but never registered */
        g_api_status = valid_info(plugin, api);
        (void)valid_class(plugin, api, false, true);

        break;
    }

    return g_api_status;
}

static hdrg_status_t policy_no_gpl(const hdrg_context_t *ctx, uint32_t index, void *user)
{
    hdrg_plugin_info_t info;

    (void)user;

    info.struct_size = sizeof(info);
    if (hdrg_context_plugin_info(ctx, index, &info) != HDRG_OK) {
        return HDRG_ERR_PLUGIN;
    }

    return (strstr(info.license, "GPL") != NULL) ? HDRG_ERR_PLUGIN : HDRG_OK;
}

static void last_plugin(hdrg_context_t *ctx, hdrg_plugin_info_t *info)
{
    info->struct_size = sizeof(*info);
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_context_plugin_info(ctx, hdrg_context_plugin_count(ctx) - 1u, info));
}

/* the context's filter of id, and its plugin's index */
static void builtin_filter(hdrg_context_t *ctx, const char *id, hdrg_filter_info_t *filter,
                           uint32_t *plugin)
{
    uint32_t i, count;

    count = hdrg_context_filter_count(ctx);
    filter->struct_size = sizeof(*filter);

    for (i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_filter_info(ctx, i, filter));

        if (strcmp(filter->id, id) == 0) {
            *plugin = filter->plugin_index;

            return;
        }
    }

    TEST_FAIL_MESSAGE(id);
}

static void expect_rejected(hdrg_plugin_init_fn_t init, uint32_t which, hdrg_status_t api_status)
{
    hdrg_plugin_info_t info;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));

    g_case = which;
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDRG_ERR_PLUGIN, hdrg_context_register_plugin(ctx, init),
                                  "registration must fail");
    TEST_ASSERT_EQUAL_INT_MESSAGE(api_status, g_api_status, "status of the bad call");

    last_plugin(ctx, &info);
    TEST_ASSERT_FALSE(info.accepted);
    TEST_ASSERT_EQUAL_UINT32(HDRG_REASON_REJECTED, info.reason);
    TEST_ASSERT_TRUE(hdrg_context_error(ctx)[0] != '\0');

    hdrg_context_destroy(ctx);
}

/* converts the test image to out_w x out_h with the SCALE filter id (NULL = default) */
static void convert_scaled(const char *plugin_path, const char *id, const char *offset,
                           uint8_t threads, uint32_t out_w, uint32_t out_h, uint8_t **output,
                           size_t *output_size)
{
    hdrg_params_t params;
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_options_t *options;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, threads));

    if (plugin_path != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_load_plugin(ctx, plugin_path));
    }

    if (id != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));

        if (offset != NULL) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "offset", offset));
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, id, options));
        hdrg_options_destroy(options);
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    encode.output_width = out_w;
    encode.output_height = out_h;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3;

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, &params, NULL, &encode, output, output_size));

    hdrg_context_destroy(ctx);
}

/* the next built-in plugin in the registration order: its id, internal and accepted */
static void tp_next_builtin(hdrg_context_t *ctx, uint32_t *index, const char *id,
                            hdrg_plugin_info_t *info)
{
    info->struct_size = sizeof(*info);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_plugin_info(ctx, *index, info));
    TEST_ASSERT_EQUAL_STRING(id, info->id);
    TEST_ASSERT_TRUE(info->internal && info->accepted);
    (*index)++;
}

/* the index of the plugin of this id */
static uint32_t tp_plugin_of(hdrg_context_t *ctx, const char *id)
{
    hdrg_plugin_info_t info;
    uint32_t i, count;

    count = hdrg_context_plugin_count(ctx);
    info.struct_size = sizeof(info);

    for (i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_plugin_info(ctx, i, &info));

        if (strcmp(info.id, id) == 0) {
            return i;
        }
    }

    TEST_FAIL_MESSAGE(id);

    return UINT32_MAX;
}

/* a built-in class (builtin_filter's) of the plugin of this id */
static void tp_class_of(hdrg_context_t *ctx, const char *id, const char *plugin_id,
                        hdrg_filter_info_t *filter)
{
    uint32_t plugin;

    builtin_filter(ctx, id, filter, &plugin);
    TEST_ASSERT_EQUAL_UINT32(tp_plugin_of(ctx, plugin_id), plugin);
}

/* the built-in plugins in the order registered: the build's filters' (src/plugins), the codecs' */
static void test_builtin_listed(void)
{
    hdrg_plugin_info_t info;
    hdrg_filter_info_t filter;
    hdrg_context_t *ctx;
    uint32_t k;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_UINT32(TP_BUILTINS, hdrg_context_plugin_count(ctx));
    TEST_ASSERT_EQUAL_UINT32(TP_CLASSES, hdrg_context_filter_count(ctx));

    k = 0;

    if (TP_HAS(BILINEAR) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.scale.bilinear", &info);
        TEST_ASSERT_EQUAL_STRING("Apache-2.0 OR MIT", info.license);
        tp_class_of(ctx, "hdrg.scale.bilinear", "hdrg.scale.bilinear", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_SCALE, filter.stages);
        TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_DETERMINISTIC) != 0);
    }

    /* the polyphase scalers: one plugin, two classes (and Lanczos on each GPU backend) */
    tp_next_builtin(ctx, &k, "hdrg.scale.polyphase", &info);
    TEST_ASSERT_EQUAL_UINT32(2u + TP_BACKENDS, info.filter_count);
    tp_class_of(ctx, "hdrg.scale.lanczos", "hdrg.scale.polyphase", &filter);
    TEST_ASSERT_EQUAL_UINT32(2, filter.option_count);
    TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_GPU_FRAMES) == 0);
    tp_class_of(ctx, "hdrg.scale.bicubic", "hdrg.scale.polyphase", &filter);
    TEST_ASSERT_EQUAL_UINT32(3, filter.option_count);
    TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_BANDS) != 0);

    /* the PREFILTER stage's: the noise reduction, deblocking, deringing, defringing, debanding */
    if (TP_HAS(DENOISE) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.denoise.spatial", &info);
        tp_class_of(ctx, "hdrg.denoise.spatial", "hdrg.denoise.spatial", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_PREFILTER, filter.stages);
        TEST_ASSERT_EQUAL_UINT32(3, filter.option_count);
    }

    if (TP_HAS(DEBLOCK) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.deblock", &info);
        tp_class_of(ctx, "hdrg.deblock", "hdrg.deblock", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_PREFILTER, filter.stages);
        TEST_ASSERT_EQUAL_UINT32(1, filter.option_count);
    }

    if (TP_HAS(DERING) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.dering", &info);
        tp_class_of(ctx, "hdrg.dering", "hdrg.dering", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_PREFILTER, filter.stages);
    }

    if (TP_HAS(DEFRINGE) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.defringe", &info);
        tp_class_of(ctx, "hdrg.defringe", "hdrg.defringe", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_PREFILTER, filter.stages);
        TEST_ASSERT_EQUAL_UINT32(1, filter.option_count);
    }

    if (TP_HAS(DEBAND) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.deband", &info);
        tp_class_of(ctx, "hdrg.deband", "hdrg.deband", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_PREFILTER, filter.stages);
        TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_GPU_FRAMES) == 0);
    }

    /* the temporal noise reduction: a time window of 2 frames each way */
    if (TP_HAS(MCTF) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.mctf", &info);
        tp_class_of(ctx, "hdrg.mctf", "hdrg.mctf", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_PREFILTER, filter.stages);
        TEST_ASSERT_EQUAL_UINT32(3, filter.option_count);
    }

    /* the frame interpolation: the repeat, the blend and the motion compensated one */
    if (TP_HAS(INTERP) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.interp", &info);
        TEST_ASSERT_EQUAL_UINT32(2u + TP_INTERP_MC * (1u + TP_BACKENDS), info.filter_count);
        tp_class_of(ctx, "hdrg.interp.repeat", "hdrg.interp", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_INTERP, filter.stages);
    }

    /* FSR 1, AMD's (MIT) */
    if (TP_HAS(FSR) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.scale.fsr", &info);
        TEST_ASSERT_EQUAL_STRING("MIT", info.license);
        tp_class_of(ctx, "hdrg.scale.fsr1", "hdrg.scale.fsr", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_SCALE, filter.stages);
    }

    /* NVIDIA Image Scaling (MIT) */
    if (TP_HAS(NIS) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.scale.nvidia", &info);
        TEST_ASSERT_EQUAL_STRING("MIT", info.license);
        tp_class_of(ctx, "hdrg.scale.nis", "hdrg.scale.nvidia", &filter);
    }

    /* the library's edge-adaptive upscale */
    if (TP_HAS(EDGE) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.scale.edge", &info);
        TEST_ASSERT_EQUAL_STRING("Apache-2.0 OR MIT", info.license);
        tp_class_of(ctx, "hdrg.scale.edge", "hdrg.scale.edge", &filter);
    }

    /* the ANALYZE stage's learned check of the judged lights */
    if (TP_HAS(PREDICT) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.analyze.predict", &info);
        tp_class_of(ctx, "hdrg.analyze.predict", "hdrg.analyze.predict", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_ANALYZE, filter.stages);
        TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_DETERMINISTIC) != 0);
    }

    /* the STATS stage's statistics across a session's frames */
    if (TP_STATS != 0) {
        tp_next_builtin(ctx, &k, "hdrg.stats.temporal", &info);
        tp_class_of(ctx, "hdrg.stats.temporal", "hdrg.stats.temporal", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_STATS, filter.stages);
    }

#if defined(HDRG_WITH_METAL)
    /* MetalFX's scaler: a plugin of its own after the filters' */
    tp_next_builtin(ctx, &k, "hdrg.metalfx", &info);
    tp_class_of(ctx, "hdrg.metal.metalfx", "hdrg.metalfx", &filter);
    TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_GPU_FRAMES) != 0);
#endif

    /* the built-in codecs after the filters: no filters of theirs */
    tp_next_builtin(ctx, &k, "hdrg.jpeg", &info);
    TEST_ASSERT_EQUAL_UINT32(0, info.filter_count);
    TEST_ASSERT_EQUAL_UINT32(1, info.codec_count);

    if (TP_HAS(PNG) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.png", &info);
    }

    if (TP_HAS(GIF) != 0) {
        tp_next_builtin(ctx, &k, "hdrg.gif", &info);
    }

    TEST_ASSERT_EQUAL_UINT32(TP_CODECS, hdrg_context_codec_count(ctx));

    /* the GPU classes in their family's plugin: the library's device frames */
#if defined(HDRG_WITH_WEBGPU)
    if (TP_HAS(DEBAND) != 0) {
        tp_class_of(ctx, "hdrg.webgpu.deband", "hdrg.deband", &filter);
        TEST_ASSERT_EQUAL_UINT32(HDRG_STAGE_PREFILTER, filter.stages);
        TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_ASYNC) != 0);
        TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_GPU_FRAMES) != 0);
    }

#endif
#if defined(HDRG_WITH_METAL)
    tp_class_of(ctx, "hdrg.metal.lanczos", "hdrg.scale.polyphase", &filter);
    TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_GPU_FRAMES) != 0);

#endif
#if defined(HDRG_WITH_VULKAN)
    if (TP_HAS(MCTF) != 0) {
        tp_class_of(ctx, "hdrg.vulkan.mctf", "hdrg.mctf", &filter);
        TEST_ASSERT_TRUE((filter.flags & HDRG_FILTER_GPU_FRAMES) != 0);
    }
#endif

    /* a struct_size from another version is refused */
    info.struct_size = sizeof(info) - 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_context_plugin_info(ctx, 0, &info));

    hdrg_context_destroy(ctx);
}

static void test_malformed_reports_rejected(void)
{
    uint32_t i;

    memset(g_big, 'a', sizeof(g_big));

    for (i = 0; i < 33; i++) {
        expect_rejected(bad_init, i, HDRG_ERR_INVALID_ARG);
    }

    for (i = 0; i < 3; i++) {
        expect_rejected(incomplete_init, i, HDRG_OK);
    }

    for (i = 0; i < 2; i++) {
        expect_rejected(async_bad_init, i, HDRG_ERR_INVALID_ARG);
    }

    expect_rejected(frames_bad_init, 0, HDRG_ERR_INVALID_ARG);
    expect_rejected(sequence_bad_init, 0, HDRG_ERR_INVALID_ARG);
}

/*
 * a plugin that reports itself unavailable: registered, not accepted, unloaded at once (its
 * deinit once), its filters not to be had
 */
static void test_unavailable_plugin(void)
{
    hdrg_plugin_info_t info;
    hdrg_context_t *ctx;
    char id[160];

    g_deinits = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, unavailable_init));
    last_plugin(ctx, &info);
    TEST_ASSERT_FALSE(info.accepted);
    TEST_ASSERT_EQUAL_UINT32(HDRG_PLUGIN_UNAVAILABLE, info.status);
    TEST_ASSERT_EQUAL_UINT32(HDRG_REASON_NO_DEVICE, info.reason);
    TEST_ASSERT_EQUAL_STRING("needs a device", info.status_message);
    TEST_ASSERT_EQUAL_UINT32(1, g_deinits);

    snprintf(id, sizeof(id), "%s.scale", g_id);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND,
                          hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, id, NULL));

    /* its id taken by none: the same id registers */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, report_init));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, g_deinits);
}

static void test_frozen_after_entry_point(void)
{
    hdrg_plugin_info_t info;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, good_init));

    /* a plugin keeping the handles cannot change what was validated */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE,
                          g_saved_api->plugin_set_info(g_saved_plugin, TP_LIT("org.evil.x"),
                                                       TP_LIT("x"), TP_LIT("1"), NULL, 0));
    TEST_ASSERT_NULL(g_saved_api->class_create(g_saved_plugin, TP_LIT("org.evil.x")));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE,
                          g_saved_api->class_set_flags(g_saved_class, HDRG_FILTER_PURE));

    /* the same id twice is refused (its classes' ids, and its own without any) */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, hdrg_context_register_plugin(ctx, good_init));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, hdrg_context_register_plugin(ctx, report_init));
    last_plugin(ctx, &info);
    TEST_ASSERT_FALSE(info.accepted);
    TEST_ASSERT_EQUAL_STRING("a plugin with the same id is already registered",
                             info.status_message);

    hdrg_context_destroy(ctx);
}

static void test_policy_rejects(void)
{
    hdrg_plugin_info_t info;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_set_plugin_policy(ctx, policy_no_gpl, NULL));

    g_license = "GPL-3.0-only";
    g_id = "org.test.gpl";
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, hdrg_context_register_plugin(ctx, good_init));

    last_plugin(ctx, &info);
    TEST_ASSERT_FALSE(info.accepted);
    TEST_ASSERT_EQUAL_UINT32(HDRG_REASON_REJECTED, info.reason);
    TEST_ASSERT_EQUAL_STRING("GPL-3.0-only", info.license);

    /* the rejected plugin's filter is not usable */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE,
                                                                      "org.test.gpl.scale", NULL));

    g_license = "MIT";
    g_id = "org.test.good";
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, good_init));

    hdrg_context_destroy(ctx);
}

/* runs a RECURSIVE session of 6 frames through ctx's STATS filter: the outputs' anchors */
static hdrg_status_t stats_session(hdrg_context_t *ctx, float *anchors, uint32_t *latency)
{
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, received;
    uint8_t *rows;

    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    hdrg_session_config_init(&config);
    config.temporal = HDRG_TEMPORAL_RECURSIVE;

    status = hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session);
    if (status != HDRG_OK) {
        return status;
    }

    *latency = hdrg_session_latency(session);
    rows = (uint8_t *)malloc((size_t)TP_W * TP_H * 3u);
    TEST_ASSERT_NOT_NULL(rows);
    memset(&in, 0, sizeof(in));
    in.planes[0] = g_pixels;
    in.strides[0] = (size_t)TP_W * 3u;
    memset(&out, 0, sizeof(out));
    out.planes[0] = rows;
    out.strides[0] = (size_t)TP_W * 3u;
    sent = 0;
    received = 0;

    while (received < 6u) {
        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < 6u) ? &in : NULL));
            sent++;

            continue;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        anchors[received++] = out.anchor_luma;
    }

    hdrg_session_destroy(session);
    free(rows);

    return HDRG_OK;
}

/*
 * The STATS stage through an app's filter, whatever the build's built-in one: every frame as sent
 * and as it renders, in order, the queue after it, its window in the session's latency, the
 * anchor it renders with; a STATS class with the frames' callbacks refused
 */
static void test_stats_stage(void)
{
    hdrg_context_t *ctx;
    uint32_t k, latency;
    float anchors[6];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT((HDRG_BUILTIN_STATS != 0) ? HDRG_OK : HDRG_ERR_UNSUPPORTED,
                          stats_session(ctx, anchors, &latency));

    g_stats_bad = false;
    memset(&g_stats, 0, sizeof(g_stats));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, stats_init));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_STATS, "org.test.stats.half", NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, stats_session(ctx, anchors, &latency));
    TEST_ASSERT_TRUE(g_stats.session_seen);
    TEST_ASSERT_EQUAL_UINT32(2, latency);
    TEST_ASSERT_EQUAL_UINT32(6, g_stats.fades);
    TEST_ASSERT_EQUAL_UINT32(6, g_stats.frames);
    TEST_ASSERT_EQUAL_UINT32(6, g_stats.renders);
    TEST_ASSERT_EQUAL_UINT32(6, g_stats.fits);
    TEST_ASSERT_EQUAL_UINT32(6, g_stats.rendered);
    TEST_ASSERT_TRUE(g_stats.queued > 0u);
    TEST_ASSERT_EQUAL_UINT32(0, g_stats.faults);

    /* the output's anchor the frame's own (out.anchor_luma: before the stage) */
    for (k = 0; k < 6u; k++) {
        TEST_ASSERT_TRUE(anchors[k] > 0.0f);
    }

    hdrg_context_destroy(ctx);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    g_stats_bad = true;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, hdrg_context_register_plugin(ctx, stats_init));
    g_stats_bad = false;
    hdrg_context_destroy(ctx);
}

/* converts a 256 x 192 test image with the light predictor's amount (*output: hdrg_free) */
static hdrg_status_t analyze_convert(hdrg_context_t *ctx, float amount, uint8_t **output,
                                     size_t *output_size)
{
    hdrg_params_t params;
    hdrg_raw_image_t raw;
    hdrg_status_t status;
    uint8_t *pixels;

    pixels = hdrg_test_image(256, 192);
    TEST_ASSERT_NOT_NULL(pixels);
    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = 256;
    raw.height = 192;
    raw.bit_depth = 8;
    raw.stride = 256u * 3u;
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.light_predictor = amount;
    *output = NULL;

    status = hdrg_convert_raw(ctx, &raw, &params, NULL, NULL, output, output_size);
    free(pixels);

    return status;
}

/*
 * The ANALYZE stage: the params' light_predictor runs the built-in predictor (where the build has
 * it), an added ANALYZE filter instead (its chances lower the lights' boost: 0 takes it away), not
 * at amount 0; an ANALYZE class with the frames' callbacks refused
 */
static void test_analyze_stage(void)
{
    hdrg_context_t *ctx;
    uint8_t *rules, *zero, *builtin;
    size_t rules_size, zero_size, builtin_size;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, analyze_convert(ctx, 0.0f, &rules, &rules_size));
    TEST_ASSERT_EQUAL_INT(HDRG_TEST_BUILTIN_STATUS(PREDICT),
                          analyze_convert(ctx, 1.0f, &builtin, &builtin_size));
    hdrg_free(builtin);

    g_analyze_bad = false;
    g_analyzed = 0;
    g_analyze_faults = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, analyze_init));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_ANALYZE, "org.test.analyze.zero", NULL));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, HDRG_STAGE_ANALYZE, "hdrg.scale.lanczos", NULL));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, analyze_convert(ctx, 0.0f, &zero, &zero_size));
    TEST_ASSERT_EQUAL_UINT32(0, g_analyzed);
    TEST_ASSERT_TRUE(zero_size == rules_size && memcmp(zero, rules, rules_size) == 0);
    hdrg_free(zero);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, analyze_convert(ctx, 1.0f, &zero, &zero_size));
    TEST_ASSERT_TRUE(g_analyzed > 0u);
    TEST_ASSERT_EQUAL_UINT32(0, g_analyze_faults);
    TEST_ASSERT_FALSE(zero_size == rules_size && memcmp(zero, rules, rules_size) == 0);
    hdrg_free(zero);
    hdrg_free(rules);
    hdrg_context_destroy(ctx);

    /* the frames' callbacks on an ANALYZE class */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    g_analyze_bad = true;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, hdrg_context_register_plugin(ctx, analyze_init));
    g_analyze_bad = false;
    hdrg_context_destroy(ctx);
}

/*
 * the context's GPU device of each kind is the same native device and queue (and WebGPU thread)
 * for every plugin of a context, another version's or a smaller struct refused; a WebGPU share's
 * run runs (by the context's end)
 */
static void test_gpu_shares(void)
{
    static const char *const ids[2] = {"org.test.layer.a.scale", "org.test.layer.b.scale"};
    hdrg_encode_params_t encode;
    hdrg_context_desc_t desc;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    uint32_t k;
    uint8_t *output;
    size_t output_size;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3;
    hdrg_context_desc_init(&desc);
    desc.gpu = HDRG_GPU_AUTO;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(&desc, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, layer_init_a));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, layer_init_b));
    memset(g_shares, 0, sizeof(g_shares));
    hdrg_atomic_u32_init(&g_share_ran, 0u);
    g_share_runs = 0;
    hdrg_encode_params_init(&encode);
    encode.output_width = TP_W * 2u;
    encode.output_height = TP_H * 2u;

    for (g_share_plugin = 0; g_share_plugin < 2u; g_share_plugin++) {
        g_share_other = HDRG_OK;
        g_share_small = HDRG_OK;
        hdrg_context_clear_filters(ctx);
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, ids[g_share_plugin], NULL));
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, hdrg_convert_raw(ctx, &raw, NULL, NULL, &encode,
                                                                     &output, &output_size));
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, g_share_other);
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED, g_share_small);
    }

    for (k = 0; k < HDRG_DEVICE_KINDS; k++) {
        TEST_ASSERT_EQUAL_PTR(g_shares[0][k].device, g_shares[1][k].device);
        TEST_ASSERT_EQUAL_PTR(g_shares[0][k].queue, g_shares[1][k].queue);
        TEST_ASSERT_EQUAL_PTR(g_shares[0][k].instance, g_shares[1][k].instance);
        TEST_ASSERT_EQUAL_PTR(g_shares[0][k].user, g_shares[1][k].user);
        TEST_ASSERT_TRUE(g_shares[0][k].device == NULL || g_shares[0][k].kind == k + 1u);
    }

    TEST_ASSERT_NULL(g_shares[0][HDRG_DEVICE_CUDA - 1u].device);

    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_UINT32(g_share_runs, hdrg_atomic_u32_load_acquire(&g_share_ran));
}

/* a codec's reader and writer read the pointers of their plugin and class back */
static void test_codec_users(void)
{
    static const uint8_t input[16] = {'T', 'P', 'C', 'O', 'D', 'E', 'C', '1'};
    hdrg_encode_params_t encode;
    hdrg_context_t *ctx;
    hdrg_params_t params;
    uint8_t *output;
    size_t output_size;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, codec_init));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_set_writer(ctx, "org.test.codec.io", NULL));
    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    hdrg_encode_params_init(&encode);
    g_codec_seen = 0;
    output = NULL;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_convert(ctx, input, sizeof(input), &params, NULL, &encode,
                                                &output, &output_size));
    TEST_ASSERT_EQUAL_UINT32(2, g_codec_seen);
    TEST_ASSERT_EQUAL_size_t(5, output_size);
    TEST_ASSERT_EQUAL_MEMORY("TPOUT", output, 5);

    hdrg_free(output);
    hdrg_context_destroy(ctx);
}

static void test_options_validated(void)
{
    hdrg_options_t *options;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, good_init));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_options_set(options, "Bad-Key", "1"));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_options_set(options, "taps", "\x1B[2J"));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "tapz", "3"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_NOT_FOUND,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));
    TEST_ASSERT_NOT_NULL(strstr(hdrg_context_error(ctx), "tapz"));
    hdrg_options_destroy(options);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "taps", "x3"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "taps", "9"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));

    /* signed values: within the range with a +, a - below it refused (int and float) */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "taps", "-3"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "taps", "+3"));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "sharp", "-0.25"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "sharp", "1.5"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "sharp", "+0.25"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));
    hdrg_context_clear_filters(ctx);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "taps", "4"));

    /* more than 15 significant digits (the zeros between them count): refused, never rounded */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "sharp", "0.1234567890123456"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "sharp", "1.5e-1"));
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", options));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_STATE,
        hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", NULL));
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_INVALID_ARG,
        hdrg_context_add_filter(ctx, UINT32_C(0x10), "org.test.good.scale", NULL));

    hdrg_context_clear_filters(ctx);
    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_SCALE, "org.test.good.scale", NULL));

    hdrg_options_destroy(options);
    hdrg_context_destroy(ctx);
}

static void test_dynamic_load(void)
{
#if defined(HDRG_TEST_PLUGIN) && defined(HDRG_TEST_PLUGIN_NEWER) && defined(HDRG_TEST_PLUGIN_OLDER)
    const char *ref, *text;
    hdrg_plugin_info_t info;
    hdrg_component_info_t component;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_load_plugin(ctx, HDRG_TEST_PLUGIN));
    last_plugin(ctx, &info);
    TEST_ASSERT_EQUAL_STRING("org.hdrgrading.test", info.id);
    TEST_ASSERT_EQUAL_STRING("0.1.0-test+1", info.version);
    TEST_ASSERT_EQUAL_UINT32(HDRG_PLUGIN_ABI_VERSION, info.abi_version);
    TEST_ASSERT_EQUAL_UINT32(HDRG_PLUGIN_DEGRADED, info.status);
    TEST_ASSERT_EQUAL_UINT32(HDRG_REASON_NO_DEVICE, info.reason);
    TEST_ASSERT_EQUAL_STRING("no GPU, running on the CPU", info.status_message);
    TEST_ASSERT_EQUAL_UINT32(2, info.filter_count);
    TEST_ASSERT_TRUE(info.accepted);
    TEST_ASSERT_FALSE(info.internal);

    component.struct_size = sizeof(component);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_plugin_component(ctx, TP_BUILTINS, 0, &component));
    TEST_ASSERT_EQUAL_STRING("CC-BY-4.0", component.license);
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_context_plugin_license_text(ctx, TP_BUILTINS, 0, &ref, &text));
    TEST_ASSERT_EQUAL_STRING("LicenseRef-Test-Data", ref);

    /* newer ABI: rejected before its entry point (which would abort) runs */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED,
                          hdrg_context_load_plugin(ctx, HDRG_TEST_PLUGIN_NEWER));
    last_plugin(ctx, &info);
    TEST_ASSERT_FALSE(info.accepted);
    TEST_ASSERT_EQUAL_UINT32(HDRG_REASON_HOST_TOO_OLD, info.reason);
    TEST_ASSERT_EQUAL_UINT32(HDRG_PLUGIN_ABI_VERSION + 1u, info.abi_version);

    /* an ABI older than the oldest one loaded: rejected the same way */
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_UNSUPPORTED,
                          hdrg_context_load_plugin(ctx, HDRG_TEST_PLUGIN_OLDER));
    last_plugin(ctx, &info);
    TEST_ASSERT_FALSE(info.accepted);
    TEST_ASSERT_EQUAL_UINT32(HDRG_REASON_PLUGIN_TOO_OLD, info.reason);
    TEST_ASSERT_EQUAL_UINT32(0, info.abi_version);

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND,
                          hdrg_context_load_plugin(ctx, "/nonexistent/hdrg_plugin.so"));
    TEST_ASSERT_EQUAL_UINT32(TP_BUILTINS + 3u, hdrg_context_plugin_count(ctx));

    hdrg_context_destroy(ctx);
#else
    TEST_IGNORE_MESSAGE("no test plugin modules in this build");
#endif
}

static void test_scale_stage_via_plugin(void)
{
#if defined(HDRG_TEST_PLUGIN)
    uint8_t *banded, *banded_t1, *whole, *offset;
    size_t banded_size, banded_t1_size, whole_size, offset_size;

    convert_scaled(HDRG_TEST_PLUGIN, "org.hdrgrading.test.nearest", NULL, 0, 2 * TP_W, 2 * TP_H,
                   &banded, &banded_size);
    convert_scaled(HDRG_TEST_PLUGIN, "org.hdrgrading.test.nearest", NULL, 1, 2 * TP_W, 2 * TP_H,
                   &banded_t1, &banded_t1_size);
    convert_scaled(HDRG_TEST_PLUGIN, "org.hdrgrading.test.whole", NULL, 0, 2 * TP_W, 2 * TP_H,
                   &whole, &whole_size);
    convert_scaled(HDRG_TEST_PLUGIN, "org.hdrgrading.test.nearest", "300", 0, 2 * TP_W, 2 * TP_H,
                   &offset, &offset_size);

    /* bands or the whole frame, any thread count: the same bytes */
    TEST_ASSERT_EQUAL_size_t(banded_size, banded_t1_size);
    TEST_ASSERT_EQUAL_MEMORY(banded, banded_t1, banded_size);
    TEST_ASSERT_EQUAL_size_t(banded_size, whole_size);
    TEST_ASSERT_EQUAL_MEMORY(banded, whole, banded_size);

    /* the option reached the plugin */
    TEST_ASSERT_TRUE(offset_size != banded_size || memcmp(offset, banded, banded_size) != 0);

    hdrg_free(banded);
    hdrg_free(banded_t1);
    hdrg_free(whole);
    hdrg_free(offset);
#else
    TEST_IGNORE_MESSAGE("no test plugin modules in this build");
#endif
}

/* a scaler class with text options (NULL key = defaults) over RGB16 pixels, whole output */
static void scale_rgb16(const char *id, const char *key, const char *value, const uint16_t *pixels,
                        uint32_t w, uint32_t h, uint32_t out_w, uint32_t out_h, uint16_t *out)
{
    const hdrg_class_t *cls;
    hdrg_filter_entry_t entry;
    hdrg_raw_image_t raw;
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_stage_t scale;
    hdrg_options_t *text;
    hdrg_context_t *ctx;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)pixels;
    raw.width = w;
    raw.height = h;
    raw.bit_depth = 16;
    raw.stride = (size_t)w * 6u;
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, &src));

    cls = hdrg_host_find_class(&ctx->host, id, strlen(id));
    TEST_ASSERT_NOT_NULL(cls);
    text = NULL;

    if (key != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &text));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(text, key, value));
    }

    entry.cls = cls;
    entry.stage = HDRG_STAGE_SCALE;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_host_resolve_options(&ctx->host, cls, text, &entry.options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_stage_open(&g_alloc, &entry, &src, out_w, out_h, out_h,
                                                        NULL, &ctx->host, &scale));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_stage_rows(&scale, NULL, 0, out_h, out, &ctx->host));

    hdrg_stage_close(&scale);
    hdrg_options_destroy(entry.options);
    hdrg_options_destroy(text);
    hdrg_source_free(&g_alloc, &src);
    hdrg_color_free(&g_alloc, &color);
    hdrg_context_destroy(ctx);
}

/* the built-in scalers through the stage executor: any band split, any worker count */
static void test_scaler_band_split_identical(void)
{
    const hdrg_class_t *cls;
    static const uint32_t sizes[3][2] = {{200, 150}, {40, 30}, {96, 64}},
                          bands[] = {7, 16, 33, 1000};
    static const char *const ids[3] = {"hdrg.scale.bilinear", "hdrg.scale.lanczos",
                                       "hdrg.scale.bicubic"};
    hdrg_filter_entry_t entry;
    hdrg_raw_image_t raw;
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_stage_t scale;
    hdrg_exec_t exec;
    hdrg_context_t *ctx;
    uint32_t s, b, w, h, y0, rows;
    uint16_t *reference, *rows_out;
    uint8_t f;
    size_t row_values;

    HDRG_TEST_REQUIRE_BUILTIN(BILINEAR);

    HDRG_TEST_REQUIRE_THREADS();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = g_pixels;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3;
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, &src));

    for (f = 0; f < 3u; f++) {
        cls = hdrg_host_find_class(&ctx->host, ids[f], strlen(ids[f]));
        TEST_ASSERT_NOT_NULL(cls);
        entry.cls = cls;
        entry.stage = HDRG_STAGE_SCALE;
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_host_resolve_options(&ctx->host, cls, NULL, &entry.options));

        for (s = 0; s < 3; s++) {
            w = sizes[s][0];
            h = sizes[s][1];
            row_values = (size_t)w * 3u;
            reference = (uint16_t *)malloc(row_values * h * sizeof(uint16_t));
            rows_out = (uint16_t *)malloc(row_values * h * sizeof(uint16_t));
            TEST_ASSERT_NOT_NULL(reference);
            TEST_ASSERT_NOT_NULL(rows_out);

            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_stage_open(&g_alloc, &entry, &src, w, h, h,
                                                                NULL, &ctx->host, &scale));
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_stage_rows(&scale, NULL, 0, h, reference, &ctx->host));
            hdrg_stage_close(&scale);

            for (b = 0; b < 4; b++) {
                TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_stage_open(&g_alloc, &entry, &src, w, h, h,
                                                                    NULL, &ctx->host, &scale));
                for (y0 = 0; y0 < h; y0 += rows) {
                    rows = (h - y0 < bands[b]) ? h - y0 : bands[b];
                    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                          hdrg_stage_rows(&scale, &exec, y0, rows,
                                                          rows_out + row_values * y0, &ctx->host));
                }

                hdrg_stage_close(&scale);
                TEST_ASSERT_EQUAL_MEMORY(reference, rows_out, row_values * h * sizeof(uint16_t));
            }

            /* the same size: the source codes themselves (8-bit widened as v * 257) */
            if (w == TP_W && h == TP_H) {
                for (y0 = 0; y0 < h * row_values; y0++) {
                    TEST_ASSERT_EQUAL_UINT16((uint16_t)(g_pixels[y0] * 257u), reference[y0]);
                }
            }

            free(reference);
            free(rows_out);
        }

        hdrg_options_destroy(entry.options);
    }

    hdrg_source_free(&g_alloc, &src);
    hdrg_pool_destroy(exec.pool);
    hdrg_context_destroy(ctx);
}

/*
 * Lanczos / bicubic: a flat picture stays flat at any size, an upscaled
 * step stays within its two levels with anti-ringing (and rings without),
 * and a downscale of one-pixel stripes is smooth (the kernel is stretched)
 * where bilinear aliases
 */
static void test_polyphase_properties(void)
{
    static const char *const ids[2] = {"hdrg.scale.lanczos", "hdrg.scale.bicubic"};
    uint32_t x, y, i;
    uint16_t *in, *out, lo, hi;
    uint8_t f;
    double s, ss, sd[3];

    HDRG_TEST_REQUIRE_BUILTIN(BILINEAR);

    in = (uint16_t *)malloc((size_t)TP_W * TP_H * 3u * sizeof(uint16_t));
    out = (uint16_t *)malloc((size_t)TP_W * 3u * TP_H * 3u * 3u * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && out != NULL);

    for (f = 0; f < 2u; f++) {
        for (i = 0; i < TP_W * TP_H * 3u; i++) {
            in[i] = 30000;
        }

        scale_rgb16(ids[f], NULL, NULL, in, TP_W, TP_H, 221, 147, out);

        for (i = 0; i < 221u * 147u * 3u; i++) {
            TEST_ASSERT_EQUAL_UINT16(30000, out[i]);
        }

        scale_rgb16(ids[f], NULL, NULL, in, TP_W, TP_H, 35, 23, out);

        for (i = 0; i < 35u * 23u * 3u; i++) {
            TEST_ASSERT_EQUAL_UINT16(30000, out[i]);
        }

        /* a step at the middle column, upscaled three times */
        for (i = 0; i < TP_W * TP_H * 3u; i++) {
            in[i] = ((i / 3u) % TP_W < TP_W / 2u) ? 10000 : 50000;
        }

        scale_rgb16(ids[f], NULL, NULL, in, TP_W, TP_H, 3u * TP_W, 3u * TP_H, out);
        lo = 65535;
        hi = 0;

        for (i = 0; i < 9u * TP_W * TP_H * 3u; i++) {
            lo = HDRG_MIN(lo, out[i]);
            hi = HDRG_MAX(hi, out[i]);
        }

        TEST_ASSERT_TRUE(lo >= 10000 && hi <= 50000);

        scale_rgb16(ids[f], "antiring", "0", in, TP_W, TP_H, 3u * TP_W, 3u * TP_H, out);
        lo = 65535;
        hi = 0;

        for (i = 0; i < 9u * TP_W * TP_H * 3u; i++) {
            lo = HDRG_MIN(lo, out[i]);
            hi = HDRG_MAX(hi, out[i]);
        }

        TEST_ASSERT_TRUE(lo < 10000 && hi > 50000);
    }

    /* one-pixel stripes, a third of the width: the spread of the output columns */
    for (y = 0; y < TP_H; y++) {
        for (x = 0; x < TP_W; x++) {
            for (i = 0; i < 3u; i++) {
                in[((size_t)y * TP_W + x) * 3u + i] = (x % 2u == 0) ? 0 : 60000;
            }
        }
    }

    for (f = 0; f < 3u; f++) {
        scale_rgb16((f == 0) ? "hdrg.scale.bilinear" : ids[f - 1u], NULL, NULL, in, TP_W, TP_H,
                    TP_W / 3u, TP_H, out);
        s = 0.0;
        ss = 0.0;

        for (x = 0; x < TP_W / 3u; x++) {
            s += (double)out[((size_t)(TP_H / 2u) * (TP_W / 3u) + x) * 3u];
            ss += (double)out[((size_t)(TP_H / 2u) * (TP_W / 3u) + x) * 3u] *
                  (double)out[((size_t)(TP_H / 2u) * (TP_W / 3u) + x) * 3u];
        }

        s /= (double)(TP_W / 3u);
        sd[f] = sqrt(ss / (double)(TP_W / 3u) - s * s);
    }

    printf("stripes / 3: column spread bilinear %.0f, lanczos %.0f, bicubic %.0f\n", sd[0], sd[1],
           sd[2]);
    TEST_ASSERT_TRUE(sd[0] > 10000.0 && sd[1] < 0.1 * sd[0] && sd[2] < 0.1 * sd[0]);

    free(in);
    free(out);
}

/* 16-bit level plus Gaussian noise of sigma codes (16-bit) on every sample */
static void noisy_rgb16(uint16_t *pixels, double sigma, bool step)
{
    uint32_t x, y;
    uint8_t c;
    double u1, u2, v;

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            for (c = 0; c < 3u; c++) {
                u1 = (double)hdrg_hash_uniform(TP_DN_KEY, x, y, c * 2u) + 1e-7;
                u2 = (double)hdrg_hash_uniform(TP_DN_KEY, x, y, c * 2u + 1u);
                v = (step && x < TP_DN_W / 2u) ? 10000.0 : (step ? 50000.0 : 30000.0);
                v += sigma * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
                pixels[((size_t)y * TP_DN_W + x) * 3u + c] =
                    (uint16_t)HDRG_CLAMP(floor(v + 0.5), 0.0, 65535.0);
            }
        }
    }
}

/*
 * a built-in PREFILTER filter over a frame in bands of band rows (its own
 * measured noise as the side data, or none), one option set (NULL = defaults);
 * whether it passed the frame through
 */
static bool prefilter_rgb16(const char *id, const uint16_t *pixels, const char *key,
                            const char *value, bool side, const hdrg_exec_t *exec, uint32_t band,
                            uint16_t *out)
{
    const hdrg_class_t *cls;
    hdrg_filter_entry_t entry;
    hdrg_raw_image_t raw;
    hdrg_color_t color;
    hdrg_source_t src;
    hdrg_stage_t stage;
    hdrg_noise_t noise;
    hdrg_options_t *text;
    hdrg_context_t *ctx;
    uint32_t y0, rows;
    bool passthrough;
    float data[HDRG_SD_NOISE_FLOATS];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, &g_alloc, 0));

    memset(&raw, 0, sizeof(raw));
    raw.pixels = (const uint8_t *)(const void *)pixels;
    raw.width = TP_DN_W;
    raw.height = TP_DN_H;
    raw.bit_depth = 16;
    raw.stride = (size_t)TP_DN_W * 6u;
    TEST_ASSERT_TRUE(hdrg_color_from_codes(HDRG_PRIMARIES_BT709, HDRG_TRANSFER_SRGB, &color));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_source_init(&g_alloc, &raw, &color, &src));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_noise_estimate(&g_alloc, NULL, &src, &noise));
    hdrg_noise_side_data(&noise, data);

    cls = hdrg_host_find_class(&ctx->host, id, strlen(id));
    TEST_ASSERT_NOT_NULL(cls);
    text = NULL;

    if (key != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &text));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(text, key, value));
    }

    entry.cls = cls;
    entry.stage = HDRG_STAGE_PREFILTER;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_host_resolve_options(&ctx->host, cls, text, &entry.options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_test_stage_open(&g_alloc, &entry, &src, TP_DN_W, TP_DN_H, band,
                                               side ? data : NULL, &ctx->host, &stage));

    for (y0 = 0; y0 < TP_DN_H; y0 += rows) {
        rows = HDRG_MIN(band, TP_DN_H - y0);
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK,
            hdrg_stage_rows(&stage, exec, y0, rows, out + (size_t)y0 * TP_DN_W * 3u, &ctx->host));
    }

    passthrough = stage.passthrough;
    hdrg_stage_close(&stage);
    hdrg_options_destroy(entry.options);
    hdrg_options_destroy(text);
    hdrg_source_free(&g_alloc, &src);
    hdrg_color_free(&g_alloc, &color);
    hdrg_context_destroy(ctx);

    return passthrough;
}

/* mean and standard deviation of the green samples over columns [x0, x1), rows 8 to h - 8 */
static void dn_region(const uint16_t *pixels, uint32_t x0, uint32_t x1, double *mean, double *sd)
{
    uint32_t x, y;
    double s, ss, n, v;

    s = 0.0;
    ss = 0.0;
    n = 0.0;

    for (y = 8; y < TP_DN_H - 8u; y++) {
        for (x = x0; x < x1; x++) {
            v = (double)pixels[((size_t)y * TP_DN_W + x) * 3u + 1u];
            s += v;
            ss += v * v;
            n += 1.0;
        }
    }

    *mean = s / n;
    *sd = sqrt(ss / n - *mean * *mean);
}

/*
 * A gray-ish gradient with 8 x 8 blocks offset by up to +-600 (16-bit) as
 * from a coarse DC quantizer, noise of 60, and a real step of 20000 at
 * TP_DB_EDGE (on the grid); blocks false: no offsets
 */
static void blocky_rgb16(uint16_t *pixels, bool blocks)
{
    uint32_t x, y;
    uint8_t c;
    double u1, u2, v;

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            for (c = 0; c < 3u; c++) {
                u1 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, c * 2u) + 1e-7;
                u2 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, c * 2u + 1u);
                v = 20000.0 + 40.0 * (double)x + 2000.0 * (double)c +
                    ((x >= TP_DB_EDGE) ? 20000.0 : 0.0) +
                    60.0 * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);

                if (blocks) {
                    v += ((double)hdrg_hash_uniform(TP_DB_KEY, x / 8u, y / 8u, 7u) - 0.5) * 1200.0;
                }

                pixels[((size_t)y * TP_DN_W + x) * 3u + c] =
                    (uint16_t)HDRG_CLAMP(floor(v + 0.5), 0.0, 65535.0);
            }
        }
    }
}

/* mean |step| of the green samples across the 8-pixel grid and inside it, the real edge left out */
static void db_steps(const uint16_t *pixels, double *grid, double *inner)
{
    uint32_t x, y;
    double g, n_grid, n_inner, step;

    *grid = 0.0;
    *inner = 0.0;
    n_grid = 0.0;
    n_inner = 0.0;

    for (y = 0; y + 1u < TP_DN_H; y++) {
        for (x = 0; x + 1u < TP_DN_W; x++) {
            g = (double)pixels[((size_t)y * TP_DN_W + x) * 3u + 1u];

            if (x + 1u != TP_DB_EDGE) {
                step = fabs((double)pixels[((size_t)y * TP_DN_W + x + 1u) * 3u + 1u] - g);
                *((x % 8u == 7u) ? grid : inner) += step;
                *((x % 8u == 7u) ? &n_grid : &n_inner) += 1.0;
            }

            step = fabs((double)pixels[((size_t)(y + 1u) * TP_DN_W + x) * 3u + 1u] - g);
            *((y % 8u == 7u) ? grid : inner) += step;
            *((y % 8u == 7u) ? &n_grid : &n_inner) += 1.0;
        }
    }

    *grid /= n_grid;
    *inner /= n_inner;
}

/*
 * hdrg.deblock (PREFILTER): lowers the steps across the grid of
 * a blocky frame, keeps a real edge on the grid and the samples no boundary
 * reaches, keeps the level, passes a frame without blocks, without the side
 * data or at strength 0 unchanged, and gives the same bytes for any band
 * split and worker count
 */
static void test_deblock(void)
{
    static const uint32_t bands[3] = {7, 16, 1000};
    const size_t samples = (size_t)TP_DN_W * TP_DN_H * 3u;
    hdrg_exec_t exec;
    uint32_t x, y;
    uint16_t *in, *out, *again;
    uint8_t b;
    double grid_in, inner_in, grid, inner, grid2, mean_in, mean, sd;

    HDRG_TEST_REQUIRE_BUILTIN(DEBLOCK);

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    out = (uint16_t *)malloc(samples * sizeof(uint16_t));
    again = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && out != NULL && again != NULL);

    blocky_rgb16(in, true);
    db_steps(in, &grid_in, &inner_in);
    TEST_ASSERT_FALSE(prefilter_rgb16("hdrg.deblock", in, NULL, NULL, true, NULL, 1000, out));
    db_steps(out, &grid, &inner);
    (void)prefilter_rgb16("hdrg.deblock", in, "strength", "2", true, NULL, 1000, again);
    db_steps(again, &grid2, &inner);
    printf("deblock: grid step %.0f -> %.0f (strength 1), %.0f (strength 2); inside %.0f\n",
           grid_in, grid, grid2, inner_in);

    TEST_ASSERT_TRUE(grid_in > 2.0 * inner_in);
    TEST_ASSERT_TRUE(grid < 0.6 * grid_in && grid2 <= grid);

    for (y = 0; y < TP_DN_H; y++) {
        TEST_ASSERT_TRUE((double)out[((size_t)y * TP_DN_W + TP_DB_EDGE) * 3u + 1u] -
                             (double)out[((size_t)y * TP_DN_W + TP_DB_EDGE - 1u) * 3u + 1u] >
                         18000.0);

        for (x = 0; x < TP_DN_W; x++) {
            if ((x % 8u == 3u || x % 8u == 4u) && (y % 8u == 3u || y % 8u == 4u)) {
                TEST_ASSERT_EQUAL_MEMORY(in + ((size_t)y * TP_DN_W + x) * 3u,
                                         out + ((size_t)y * TP_DN_W + x) * 3u,
                                         3u * sizeof(uint16_t));
            }
        }
    }

    dn_region(in, 0, TP_DN_W, &mean_in, &sd);
    dn_region(out, 0, TP_DN_W, &mean, &sd);
    TEST_ASSERT_TRUE(fabs(mean - mean_in) < 0.002 * mean_in);

    /* nothing to measure against, nothing to do, no blocks: passed through */
    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.deblock", in, NULL, NULL, false, NULL, 1000, again));
    TEST_ASSERT_EQUAL_MEMORY(in, again, samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.deblock", in, "strength", "0", true, NULL, 1000, again));
    TEST_ASSERT_EQUAL_MEMORY(in, again, samples * sizeof(uint16_t));
    blocky_rgb16(again, false);
    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.deblock", again, NULL, NULL, true, NULL, 1000, out));
    TEST_ASSERT_EQUAL_MEMORY(again, out, samples * sizeof(uint16_t));

    /* any band split and worker count */
    HDRG_TEST_REQUIRE_THREADS();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;
    (void)prefilter_rgb16("hdrg.deblock", in, NULL, NULL, true, NULL, 1000, out);

    for (b = 0; b < 3u; b++) {
        (void)prefilter_rgb16("hdrg.deblock", in, NULL, NULL, true, &exec, bands[b], again);
        TEST_ASSERT_EQUAL_MEMORY(out, again, samples * sizeof(uint16_t));
    }

    hdrg_pool_destroy(exec.pool);
    free(in);
    free(out);
    free(again);
}

/*
 * Flat 30000 with a dark line at x = TP_DB_EDGE, + 1 and, when ringing, a
 * swing of +-1000 changing sign every pixel at x = TP_DB_EDGE + 4 .. + 7
 * (inside the line's blocks); noise of sigma everywhere
 */
static void ringing_rgb16(uint16_t *pixels, bool ringing, double sigma)
{
    uint32_t x, y;
    uint8_t c;
    double u1, u2, v;

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            for (c = 0; c < 3u; c++) {
                u1 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, c * 2u + 16u) + 1e-7;
                u2 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, c * 2u + 17u);
                v = (x == TP_DB_EDGE || x == TP_DB_EDGE + 1u) ? 3000.0 : 30000.0;
                v += (ringing && x >= TP_DB_EDGE + 4u && x <= TP_DB_EDGE + 7u)
                         ? (((x + y) % 2u == 0) ? 1000.0 : -1000.0)
                         : 0.0;
                v += sigma * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
                pixels[((size_t)y * TP_DN_W + x) * 3u + c] =
                    (uint16_t)HDRG_CLAMP(floor(v + 0.5), 0.0, 65535.0);
            }
        }
    }
}

/* mean |green - 30000| over the ringing columns, the two border rows left out */
static double dr_swing(const uint16_t *pixels)
{
    uint32_t x, y;
    double sum, n;

    sum = 0.0;
    n = 0.0;

    for (y = 2; y + 2u < TP_DN_H; y++) {
        for (x = TP_DB_EDGE + 4u; x <= TP_DB_EDGE + 7u; x++) {
            sum += fabs((double)pixels[((size_t)y * TP_DN_W + x) * 3u + 1u] - 30000.0);
            n += 1.0;
        }
    }

    return sum / n;
}

/*
 * ringing_rgb16 with three more lines and swings at x = 160, 192, 224, and
 * steep ramps (1800 a pixel from 8000) in two 32-pixel segments from x = 0,
 * each after a 2-pixel dark line
 */
static void ramps_rgb16(uint16_t *pixels)
{
    uint32_t x, y, k;
    uint16_t v;
    uint8_t c;

    ringing_rgb16(pixels, true, 0.0);

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            k = x % 32u;

            if (x < 64u) {
                v = (uint16_t)((k < 2u) ? 2000u : 8000u + (k - 2u) * 1800u);
            } else if (x > TP_DB_EDGE + 8u) {
                v = (uint16_t)((k < 2u) ? 3000u : 30000u);
                v = (uint16_t)((k >= 4u && k < 8u) ? (((x + y) % 2u == 0) ? 31000u : 29000u) : v);
            } else {
                continue;
            }

            for (c = 0; c < 3u; c++) {
                pixels[((size_t)y * TP_DN_W + x) * 3u + c] = v;
            }
        }
    }
}

/*
 * hdrg.dering (PREFILTER): averages out the swings around a line
 * inside its blocks, keeps the line and every block without an edge, passes
 * a frame without ringing or with noise everywhere through, and gives the
 * same bytes for any band split and worker count
 */
static void test_dering(void)
{
    static const uint32_t bands[3] = {7, 16, 1000};
    const size_t samples = (size_t)TP_DN_W * TP_DN_H * 3u;
    hdrg_exec_t exec;
    uint32_t x, y;
    uint16_t *in, *out, *again;
    uint8_t b;
    double before, after;

    HDRG_TEST_REQUIRE_BUILTIN(DERING);

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    out = (uint16_t *)malloc(samples * sizeof(uint16_t));
    again = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && out != NULL && again != NULL);

    ringing_rgb16(in, true, 0.0);
    TEST_ASSERT_FALSE(prefilter_rgb16("hdrg.dering", in, NULL, NULL, true, NULL, 1000, out));
    before = dr_swing(in);
    after = dr_swing(out);
    printf("dering: swing %.0f -> %.0f\n", before, after);

    TEST_ASSERT_TRUE(after < 0.3 * before);

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            if (x / 8u != TP_DB_EDGE / 8u || x == TP_DB_EDGE || x == TP_DB_EDGE + 1u) {
                TEST_ASSERT_EQUAL_MEMORY(in + ((size_t)y * TP_DN_W + x) * 3u,
                                         out + ((size_t)y * TP_DN_W + x) * 3u,
                                         3u * sizeof(uint16_t));
            }
        }
    }

    /* no ringing, noise everywhere: passed through */
    ringing_rgb16(again, false, 0.0);
    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.dering", again, NULL, NULL, true, NULL, 1000, out));
    TEST_ASSERT_EQUAL_MEMORY(again, out, samples * sizeof(uint16_t));
    ringing_rgb16(again, true, 900.0);
    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.dering", again, NULL, NULL, true, NULL, 1000, out));

    /* any band split and worker count */
    HDRG_TEST_REQUIRE_THREADS();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;
    (void)prefilter_rgb16("hdrg.dering", in, NULL, NULL, true, NULL, 1000, out);

    for (b = 0; b < 3u; b++) {
        (void)prefilter_rgb16("hdrg.dering", in, NULL, NULL, true, &exec, bands[b], again);
        TEST_ASSERT_EQUAL_MEMORY(out, again, samples * sizeof(uint16_t));
    }

    hdrg_pool_destroy(exec.pool);
    free(in);
    free(out);
    free(again);
}

/* Y' of a 16-bit sample, as the filters compute it (BT.709 in 1/65536) */
static int32_t df_luma(const uint16_t *s)
{
    return (int32_t)(((uint32_t)s[0] * 13933u + (uint32_t)s[1] * 46871u + (uint32_t)s[2] * 4732u +
                      32768u) >>
                     16);
}

/* a sample of Y' luma with B' - Y' = db and R' - Y' = dr */
static void df_put(uint16_t *s, int32_t luma, int32_t db, int32_t dr)
{
    s[0] = (uint16_t)(luma + dr);
    s[1] = (uint16_t)(luma - (13933 * dr + 4732 * db) / 46871);
    s[2] = (uint16_t)(luma + db);
}

/*
 * ramps_rgb16's right half (lines and swings: the ringing that turns the
 * filter on); on the left an orange field crossed by a dark line at x = 40,
 * 41, the two columns after it of its Y' but half and three quarters of its
 * chroma (bled toward the line's), and from x = 100 a teal of the same Y'
 */
static void fringe_rgb16(uint16_t *pixels)
{
    uint32_t x, y;
    uint16_t *s;

    ramps_rgb16(pixels);

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DB_EDGE; x++) {
            s = pixels + ((size_t)y * TP_DN_W + x) * 3u;

            if (x == 40u || x == 41u) {
                s[0] = 3000;
                s[1] = 3000;
                s[2] = 3000;
            } else if (x >= 100u) {
                df_put(s, TP_DF_LUMA, 8000, -15000);
            } else {
                df_put(s, TP_DF_LUMA,
                       (x == 42u) ? TP_DF_DB / 2 : ((x == 43u) ? TP_DF_DB * 3 / 4 : TP_DF_DB),
                       (x == 42u) ? TP_DF_DR / 2 : ((x == 43u) ? TP_DF_DR * 3 / 4 : TP_DF_DR));
            }
        }
    }
}

/* the mean |B' - Y' - orange's| + |R' - Y' - orange's| of columns 42 and 43 */
static double df_bleed(const uint16_t *pixels)
{
    const uint16_t *s;
    uint32_t x, y;
    double sum;

    sum = 0.0;

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 42; x <= 43u; x++) {
            s = pixels + ((size_t)y * TP_DN_W + x) * 3u;
            sum += fabs((double)s[2] - (double)df_luma(s) - TP_DF_DB) +
                   fabs((double)s[0] - (double)df_luma(s) - TP_DF_DR);
        }
    }

    return sum / (2.0 * TP_DN_H);
}

/*
 * hdrg.defringe (PREFILTER): the chroma bled next to a line comes
 * back toward its field's, Y' stays, the samples away from luma edges (an
 * isoluminant color edge among them) keep their bytes, a frame without
 * ringing and strength 0 pass, and any band split and worker count give the
 * same bytes
 */
static void test_defringe(void)
{
    static const uint32_t bands[3] = {7, 16, 1000};
    const size_t samples = (size_t)TP_DN_W * TP_DN_H * 3u;
    hdrg_exec_t exec;
    uint32_t x, y;
    uint16_t *in, *out, *again;
    uint8_t b;
    double before, after;

    HDRG_TEST_REQUIRE_BUILTIN(DEFRINGE);

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    out = (uint16_t *)malloc(samples * sizeof(uint16_t));
    again = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && out != NULL && again != NULL);

    fringe_rgb16(in);
    TEST_ASSERT_FALSE(prefilter_rgb16("hdrg.defringe", in, NULL, NULL, true, NULL, 1000, out));
    before = df_bleed(in);
    after = df_bleed(out);
    printf("defringe: bleed %.0f -> %.0f\n", before, after);

    TEST_ASSERT_TRUE(after < 0.5 * before);

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            TEST_ASSERT_TRUE(abs(df_luma(in + ((size_t)y * TP_DN_W + x) * 3u) -
                                 df_luma(out + ((size_t)y * TP_DN_W + x) * 3u)) <= 1);

            if (x >= 50u && x <= 120u) {
                TEST_ASSERT_EQUAL_MEMORY(in + ((size_t)y * TP_DN_W + x) * 3u,
                                         out + ((size_t)y * TP_DN_W + x) * 3u,
                                         3u * sizeof(uint16_t));
            }
        }
    }

    /* no ringing, strength 0: passed through */
    ringing_rgb16(again, false, 0.0);
    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.defringe", again, NULL, NULL, true, NULL, 1000, out));
    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.defringe", in, "strength", "0", true, NULL, 1000, out));
    TEST_ASSERT_EQUAL_MEMORY(in, out, samples * sizeof(uint16_t));

    /* any band split and worker count */
    HDRG_TEST_REQUIRE_THREADS();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;
    (void)prefilter_rgb16("hdrg.defringe", in, NULL, NULL, true, NULL, 1000, out);

    for (b = 0; b < 3u; b++) {
        (void)prefilter_rgb16("hdrg.defringe", in, NULL, NULL, true, &exec, bands[b], again);
        TEST_ASSERT_EQUAL_MEMORY(out, again, samples * sizeof(uint16_t));
    }

    hdrg_pool_destroy(exec.pool);
    free(in);
    free(out);
    free(again);
}

/*
 * Left (x < 120): a ramp of 60 a pixel from 20000 in 8-bit steps (one code
 * every 4.3 pixels); then a flat 30000 with a dark line at x = 160, 161;
 * from x = 200 a grain of sigma 3 codes on 30000
 */
static void steps_rgb16(uint16_t *pixels)
{
    uint32_t x, y;
    uint16_t v;
    uint8_t c;
    double u1, u2;

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            u1 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, 40u) + 1e-7;
            u2 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, 41u);

            if (x < 120u) {
                v = (uint16_t)(floor((20000.0 + 60.0 * x) / 257.0 + 0.5) * 257.0);
            } else if (x < 200u) {
                v = (uint16_t)((x == 160u || x == 161u) ? 3000u : 30000u);
            } else {
                v = (uint16_t)floor(
                    30000.0 + 771.0 * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2) + 0.5);
            }

            for (c = 0; c < 3u; c++) {
                pixels[((size_t)y * TP_DN_W + x) * 3u + c] = v;
            }
        }
    }
}

/* the mean |green - the continuous ramp| over the ramp off the frame's borders */
static double db_ramp_error(const uint16_t *pixels)
{
    uint32_t x, y;
    double sum, n;

    sum = 0.0;
    n = 0.0;

    for (y = 8; y + 8u < TP_DN_H; y++) {
        for (x = 8; x < 110u; x++) {
            sum += fabs((double)pixels[((size_t)y * TP_DN_W + x) * 3u + 1u] - (20000.0 + 60.0 * x));
            n += 1.0;
        }
    }

    return sum / n;
}

/*
 * a light grain over 30000: sigma 1.2 8-bit codes, above what quantization
 * leaves but within strength 1.5's residual ramp (only its lack of steps
 * keeps it)
 */
static void light_grain_rgb16(uint16_t *pixels)
{
    uint32_t x, y;
    uint16_t v;
    uint8_t c;
    double u1, u2;

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            u1 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, 50u) + 1e-7;
            u2 = (double)hdrg_hash_uniform(TP_DB_KEY, x, y, 51u);
            v = (uint16_t)floor(30000.0 +
                                308.4 * sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2) + 0.5);

            for (c = 0; c < 3u; c++) {
                pixels[((size_t)y * TP_DN_W + x) * 3u + c] = v;
            }
        }
    }
}

/*
 * hdrg.deband (PREFILTER): a ramp in 8-bit steps comes back
 * close to the continuous ramp, a line and the samples near it (an edge in
 * every window) and a grain (too large a residual) keep their bytes, a
 * light grain keeps them too at strength 1.5 (no steps in it), strength 0
 * and a frame grainy all over (no quiet level) pass, and any band split and
 * worker count give the same bytes
 */
static void test_deband(void)
{
    static const uint32_t bands[3] = {7, 16, 1000};
    const size_t samples = (size_t)TP_DN_W * TP_DN_H * 3u;
    hdrg_exec_t exec;
    uint32_t x, y;
    uint16_t *in, *out, *again, grain[1];
    uint8_t b;
    size_t i, kept;
    double before, after;

    HDRG_TEST_REQUIRE_BUILTIN(DEBAND);

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    out = (uint16_t *)malloc(samples * sizeof(uint16_t));
    again = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && out != NULL && again != NULL);

    steps_rgb16(in);
    TEST_ASSERT_FALSE(prefilter_rgb16("hdrg.deband", in, NULL, NULL, false, NULL, 1000, out));
    before = db_ramp_error(in);
    after = db_ramp_error(out);
    printf("deband: ramp error %.1f -> %.1f\n", before, after);

    TEST_ASSERT_TRUE(after < 0.25 * before);

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 156; x < TP_DN_W; x++) {
            if (x <= 165u || x >= 200u) {
                TEST_ASSERT_EQUAL_MEMORY(in + ((size_t)y * TP_DN_W + x) * 3u,
                                         out + ((size_t)y * TP_DN_W + x) * 3u,
                                         3u * sizeof(uint16_t));
            }
        }
    }

    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.deband", in, "strength", "0", false, NULL, 1000, again));
    TEST_ASSERT_EQUAL_MEMORY(in, again, samples * sizeof(uint16_t));

    for (y = 0; y < TP_DN_H; y++) {
        for (x = 0; x < TP_DN_W; x++) {
            grain[0] = in[((size_t)y * TP_DN_W + 200u + x % 40u) * 3u];
            again[((size_t)y * TP_DN_W + x) * 3u] = grain[0];
            again[((size_t)y * TP_DN_W + x) * 3u + 1u] = grain[0];
            again[((size_t)y * TP_DN_W + x) * 3u + 2u] = grain[0];
        }
    }

    TEST_ASSERT_TRUE(prefilter_rgb16("hdrg.deband", again, NULL, NULL, true, NULL, 1000, out));
    TEST_ASSERT_FALSE(prefilter_rgb16("hdrg.deband", in, NULL, NULL, true, NULL, 1000, out));

    light_grain_rgb16(again);
    (void)prefilter_rgb16("hdrg.deband", again, "strength", "1.5", false, NULL, 1000, out);
    kept = 0;

    for (i = 0; i < samples; i++) {
        kept += (out[i] == again[i]) ? 1u : 0u;
    }

    printf("deband: light grain, %.1f%% of the samples kept\n",
           100.0 * (double)kept / (double)samples);

    TEST_ASSERT_TRUE((double)kept > 0.95 * (double)samples);

    (void)prefilter_rgb16("hdrg.deband", in, NULL, NULL, false, NULL, 1000, out);

    /* any band split and worker count */
    HDRG_TEST_REQUIRE_THREADS();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;

    for (b = 0; b < 3u; b++) {
        (void)prefilter_rgb16("hdrg.deband", in, NULL, NULL, false, &exec, bands[b], again);
        TEST_ASSERT_EQUAL_MEMORY(out, again, samples * sizeof(uint16_t));
    }

    hdrg_pool_destroy(exec.pool);
    free(in);
    free(out);
    free(again);
}

/*
 * hdrg.dering on steep gradients, at any strength (the range follows the
 * slope, not the window): a ramp's samples whose window stays off the lines
 * keep their bytes, those next to a line move by at most one sample's step,
 * and the swings beside the other lines still go
 */
static void test_dering_gradient(void)
{
    static const uint32_t reaches[2] = {3, 7};
    const size_t samples = (size_t)TP_DN_W * TP_DN_H * 3u;
    static const char *const strengths[2] = {"1", "3"};
    uint32_t x, y, k;
    uint16_t *in, *out;
    uint8_t i;
    double offset, worst;

    HDRG_TEST_REQUIRE_BUILTIN(DERING);

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    out = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && out != NULL);

    ramps_rgb16(in);

    for (i = 0; i < 2u; i++) {
        TEST_ASSERT_FALSE(
            prefilter_rgb16("hdrg.dering", in, "strength", strengths[i], true, NULL, 1000, out));
        TEST_ASSERT_TRUE(dr_swing(out) < 0.3 * dr_swing(in));
        worst = 0.0;

        for (x = 0; x < 64u; x++) {
            k = x % 32u;

            if (k < 2u) {
                continue;
            }

            if (k >= reaches[i] + 2u && k + reaches[i] < 32u) {
                for (y = 0; y < TP_DN_H; y++) {
                    TEST_ASSERT_EQUAL_MEMORY(in + ((size_t)y * TP_DN_W + x) * 3u,
                                             out + ((size_t)y * TP_DN_W + x) * 3u,
                                             3u * sizeof(uint16_t));
                }

                continue;
            }

            offset = 0.0;

            for (y = 0; y < TP_DN_H; y++) {
                offset += (double)out[((size_t)y * TP_DN_W + x) * 3u + 1u] -
                          (double)in[((size_t)y * TP_DN_W + x) * 3u + 1u];
            }

            offset /= (double)TP_DN_H;
            worst = HDRG_MAX(worst, fabs(offset));
        }

        printf("dering %s: ramp offset next to the lines %.0f (a step 1800)\n", strengths[i],
               worst);

        TEST_ASSERT_TRUE(worst <= 1800.0);
    }

    free(in);
    free(out);
}

/*
 * a still of 8-bit pixels (TP_DN_W x TP_DN_H) through PREFILTER filters
 * added in order: the file (hdrg_free) and its size
 */
static uint8_t *convert_chain(const uint8_t *pixels, const hdrg_params_t *params,
                              const char *const *ids, const char *const *strengths, uint8_t count,
                              size_t *size)
{
    hdrg_encode_params_t encode;
    hdrg_raw_image_t raw;
    hdrg_options_t *options;
    hdrg_context_t *ctx;
    uint8_t *output, i;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));

    for (i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "strength", strengths[i]));
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, ids[i], options));
        hdrg_options_destroy(options);
    }

    hdrg_encode_params_init(&encode);
    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_DN_W;
    raw.height = TP_DN_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_DN_W * 3u;

    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_convert_raw(ctx, &raw, params, NULL, &encode, &output, size));
    hdrg_context_destroy(ctx);

    return output;
}

/*
 * The PREFILTER chain: the params' built-in deblocking and noise reduction
 * equal the two filters added in that order, and a pass-through filter in
 * front changes nothing (three filters, the frame between them alternating);
 * the stage takes HDRG_PREFILTER_MAX filters
 */
static void test_prefilter_chain(void)
{
    const size_t samples = (size_t)TP_DN_W * TP_DN_H * 3u;
    const char *const ids[3] = {"hdrg.denoise.spatial", "hdrg.deblock", "hdrg.denoise.spatial"},
                      *const strengths[3] = {"0", "1", "1"};
    hdrg_params_t params, plain;
    hdrg_context_t *ctx;
    uint16_t *wide;
    uint8_t *pixels, *want, *got, i;
    size_t i_sample, want_size, got_size;

    HDRG_TEST_REQUIRE_BUILTIN(DEBLOCK);
    HDRG_TEST_REQUIRE_BUILTIN(DENOISE);

    wide = (uint16_t *)malloc(samples * sizeof(uint16_t));
    pixels = (uint8_t *)malloc(samples);
    TEST_ASSERT_TRUE(wide != NULL && pixels != NULL);
    blocky_rgb16(wide, true);

    for (i_sample = 0; i_sample < samples; i_sample++) {
        pixels[i_sample] = (uint8_t)((wide[i_sample] + 128u) / 257u);
    }

    hdrg_params_init(&params, HDRG_PRESET_PHOTO);
    params.deblock_strength = 0.0f;
    params.deband_strength = 0.0f;
    plain = params;
    params.deblock_strength = 1.0f;
    params.grain_policy = HDRG_GRAIN_REDUCE;

    want = convert_chain(pixels, &params, NULL, NULL, 0, &want_size);
    got = convert_chain(pixels, &plain, ids + 1, strengths + 1, 2, &got_size);
    TEST_ASSERT_EQUAL_size_t(want_size, got_size);
    TEST_ASSERT_EQUAL_MEMORY(want, got, want_size);
    hdrg_free(got);

    /* the added filters replace the built-in ones */
    got = convert_chain(pixels, &params, ids, strengths, 3, &got_size);
    TEST_ASSERT_EQUAL_size_t(want_size, got_size);
    TEST_ASSERT_EQUAL_MEMORY(want, got, want_size);
    hdrg_free(got);

    /* the deblocking is at work: without it the output differs */
    plain.grain_policy = HDRG_GRAIN_REDUCE;
    got = convert_chain(pixels, &plain, NULL, NULL, 0, &got_size);
    TEST_ASSERT_TRUE(got_size != want_size || memcmp(want, got, want_size) != 0);
    hdrg_free(got);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));

    for (i = 0; i < HDRG_PREFILTER_MAX; i++) {
        TEST_ASSERT_EQUAL_INT(
            HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, "hdrg.deblock", NULL));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE,
                          hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, "hdrg.deblock", NULL));
    hdrg_context_destroy(ctx);
    hdrg_free(want);

    /*
     * a still none of the filters has anything to do for skips the stage:
     * ANIME's deblocking and deringing on a clean frame give the bytes of
     * neither (its debanding off: a clean gradient is its to work on)
     */
    blocky_rgb16(wide, false);

    for (i_sample = 0; i_sample < samples; i_sample++) {
        pixels[i_sample] = (uint8_t)((wide[i_sample] + 128u) / 257u);
    }

    hdrg_params_init(&params, HDRG_PRESET_ANIME);
    params.deband_strength = 0.0f;
    plain = params;
    plain.deblock_strength = 0.0f;
    plain.dering_strength = 0.0f;
    want = convert_chain(pixels, &plain, NULL, NULL, 0, &want_size);
    got = convert_chain(pixels, &params, NULL, NULL, 0, &got_size);
    TEST_ASSERT_EQUAL_size_t(want_size, got_size);
    TEST_ASSERT_EQUAL_MEMORY(want, got, want_size);
    hdrg_free(got);

    hdrg_free(want);
    free(wide);
    free(pixels);
}

/*
 * hdrg.denoise.spatial (PREFILTER): lowers flat noise by its
 * strength, keeps the levels of an edge two pixels away, passes a clean
 * frame and a frame without the side data unchanged, and gives the same
 * bytes for any band split and worker count
 */
static void test_denoise(void)
{
    static const uint32_t bands[3] = {7, 16, 1000};
    const size_t samples = (size_t)TP_DN_W * TP_DN_H * 3u;
    hdrg_exec_t exec;
    uint16_t *in, *out, *again;
    uint8_t b;
    double mean_in, sd_in, mean, sd, sd3, left, right;

    HDRG_TEST_REQUIRE_BUILTIN(DENOISE);

    in = (uint16_t *)malloc(samples * sizeof(uint16_t));
    out = (uint16_t *)malloc(samples * sizeof(uint16_t));
    again = (uint16_t *)malloc(samples * sizeof(uint16_t));
    TEST_ASSERT_TRUE(in != NULL && out != NULL && again != NULL);

    /* flat noise of 4 codes in 8-bit terms */
    noisy_rgb16(in, 4.0 * 257.0, false);
    dn_region(in, 16, TP_DN_W - 16u, &mean_in, &sd_in);
    (void)prefilter_rgb16("hdrg.denoise.spatial", in, NULL, NULL, true, NULL, 1000, out);
    dn_region(out, 16, TP_DN_W - 16u, &mean, &sd);
    (void)prefilter_rgb16("hdrg.denoise.spatial", in, "strength", "3", true, NULL, 1000, again);
    dn_region(again, 16, TP_DN_W - 16u, &mean, &sd3);
    printf("denoise: flat sd %.0f -> %.0f (strength 1), %.0f (strength 3)\n", sd_in, sd, sd3);

    TEST_ASSERT_TRUE(sd < 0.6 * sd_in && sd3 < 0.35 * sd_in);
    TEST_ASSERT_TRUE(fabs(mean - mean_in) < 0.005 * mean_in);

    /* without the side data, nothing to measure against: passed through */
    TEST_ASSERT_TRUE(
        prefilter_rgb16("hdrg.denoise.spatial", in, NULL, NULL, false, NULL, 1000, out));
    TEST_ASSERT_EQUAL_MEMORY(in, out, samples * sizeof(uint16_t));

    /* any band split and worker count */
    HDRG_TEST_REQUIRE_THREADS();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_pool_create(&exec.pool, &g_alloc, 7, NULL, NULL));
    exec.max_workers = 8;
    (void)prefilter_rgb16("hdrg.denoise.spatial", in, NULL, NULL, true, NULL, 1000, out);

    for (b = 0; b < 3u; b++) {
        (void)prefilter_rgb16("hdrg.denoise.spatial", in, NULL, NULL, true, &exec, bands[b], again);
        TEST_ASSERT_EQUAL_MEMORY(out, again, samples * sizeof(uint16_t));
    }

    hdrg_pool_destroy(exec.pool);

    /* an edge: the levels two pixels away stay, the flat sides get smoother */
    noisy_rgb16(in, 4.0 * 257.0, true);
    (void)prefilter_rgb16("hdrg.denoise.spatial", in, NULL, NULL, true, NULL, 1000, out);
    dn_region(out, TP_DN_W / 2u - 3u, TP_DN_W / 2u - 2u, &left, &sd);
    dn_region(out, TP_DN_W / 2u + 2u, TP_DN_W / 2u + 3u, &right, &sd);
    printf("denoise: edge at 2 px %.0f | %.0f\n", left, right);

    TEST_ASSERT_TRUE(fabs(left - 10000.0) < 200.0 && fabs(right - 50000.0) < 1000.0);
    dn_region(out, 16, TP_DN_W / 2u - 16u, &mean, &sd);
    TEST_ASSERT_TRUE(sd < 0.6 * sd_in);

    /* a clean frame measures no noise: passed through */
    noisy_rgb16(in, 0.0, true);
    TEST_ASSERT_TRUE(
        prefilter_rgb16("hdrg.denoise.spatial", in, NULL, NULL, true, NULL, 1000, out));
    TEST_ASSERT_EQUAL_MEMORY(in, out, samples * sizeof(uint16_t));

    free(in);
    free(out);
    free(again);
}

static void test_default_scaler_output_size(void)
{
    hdrg_decoded_t decoded;
    uint8_t *output, *output_t1;
    size_t output_size, output_t1_size;

    convert_scaled(NULL, NULL, NULL, 0, 150, 0, &output, &output_size);
    convert_scaled(NULL, NULL, NULL, 1, 150, 0, &output_t1, &output_t1_size);

    TEST_ASSERT_EQUAL_size_t(output_size, output_t1_size);
    TEST_ASSERT_EQUAL_MEMORY(output, output_t1, output_size);

    /* 150 x round(64 * 150 / 96) */
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_decode_any(&g_alloc, NULL, output, output_size, UINT64_MAX,
                                                   HDRG_CHROMA_FIT_LINEAR, &decoded));
    TEST_ASSERT_EQUAL_UINT32(150, decoded.width);
    TEST_ASSERT_EQUAL_UINT32(100, decoded.height);

    hdrg_decoded_free(&g_alloc, &decoded);
    hdrg_free(output);
    hdrg_free(output_t1);
}

/* a context with the lifetime plugin on the PREFILTER stage (model: NULL = the default) */
static hdrg_context_t *life_context(const char *model)
{
    hdrg_options_t *options;
    hdrg_context_t *ctx;

    memset(&g_life, 0, sizeof(g_life));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, life_init));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));

    if (model != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "model", model));
    }

    TEST_ASSERT_EQUAL_INT(
        HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, "org.test.life", options));
    hdrg_options_destroy(options);

    return ctx;
}

/*
 * An instance lives as long as the conversion (a still: configured
 * for the probe and the frame) or the session (configured per frame); the
 * plugin's user pointer reaches its filters; a string option comes with its
 * default, listed, or as set; the filters are told of the input's color
 * and bits
 */
static void test_lifetime(void)
{
    hdrg_option_info_t option;
    hdrg_filter_info_t filter;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_raw_image_t raw;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint32_t i, index, sent, received;
    uint8_t *pixels, *output, *rows;
    size_t size;

    pixels = (uint8_t *)malloc((size_t)TP_W * TP_H * 3u);
    rows = (uint8_t *)malloc((size_t)TP_W * TP_H * 3u);
    TEST_ASSERT_TRUE(pixels != NULL && rows != NULL);

    for (i = 0; i < TP_W * TP_H * 3u; i++) {
        pixels[i] = (uint8_t)((i * 7u) % 251u);
    }

    /* the string option, listed with its default */
    ctx = life_context(NULL);
    index = UINT32_MAX;

    for (i = 0; i < hdrg_context_filter_count(ctx); i++) {
        filter.struct_size = sizeof(filter);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_filter_info(ctx, i, &filter));
        index = (strcmp(filter.id, "org.test.life") == 0) ? i : index;
    }

    option.struct_size = sizeof(option);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_filter_option(ctx, index, 0, &option));
    TEST_ASSERT_EQUAL_UINT32(HDRG_OPTION_STRING, option.type);
    TEST_ASSERT_EQUAL_STRING("none.onnx", option.default_text);
    hdrg_context_destroy(ctx);

    /* a still: one instance, configured for the probe and the frame */
    ctx = life_context("models/x2.onnx");
    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &size));
    hdrg_free(output);

    TEST_ASSERT_EQUAL_UINT32(1, g_life.creates);
    TEST_ASSERT_EQUAL_UINT32(1, g_life.destroys);
    TEST_ASSERT_EQUAL_UINT32(2, g_life.configures);
    TEST_ASSERT_EQUAL_STRING("models/x2.onnx", g_life.model);
    TEST_ASSERT_EQUAL_size_t(strlen("models/x2.onnx") + 1u, g_life.needed);
    TEST_ASSERT_TRUE(g_life.user == &g_life_marker);
    TEST_ASSERT_EQUAL_UINT32(HDRG_TRANSFER_SRGB, g_life.transfer);
    TEST_ASSERT_EQUAL_UINT32(8, g_life.bits);
    TEST_ASSERT_TRUE(fabs(g_life.xy[0] - 0.64) < 1e-3 && fabs(g_life.xy[1] - 0.33) < 1e-3);
    TEST_ASSERT_TRUE(fabs(g_life.xy[4] - 0.15) < 1e-3 && fabs(g_life.xy[5] - 0.06) < 1e-3);
    TEST_ASSERT_TRUE(fabs(g_life.xy[6] - 0.3127) < 1e-3 && fabs(g_life.xy[7] - 0.3290) < 1e-3);
    hdrg_context_destroy(ctx);

    /* a session: one instance, configured per frame */
    ctx = life_context(NULL);
    raw.pixels = NULL;
    hdrg_session_config_init(&config);
    config.past = 0;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_create(ctx, HDRG_TEST_FORMAT(&raw), tp_plain(),
                                                       NULL, &config, &session));
    TEST_ASSERT_EQUAL_STRING("none.onnx", g_life.model);

    memset(&in, 0, sizeof(in));
    in.planes[0] = pixels;
    in.strides[0] = (size_t)TP_W * 3u;
    memset(&out, 0, sizeof(out));
    out.planes[0] = rows;
    out.strides[0] = (size_t)TP_W * 3u;
    sent = 0;
    received = 0;

    while (received < 5u) {
        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < 5u) ? &in : NULL));
            sent++;

            continue;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;
    }

    hdrg_session_destroy(session);
    TEST_ASSERT_EQUAL_UINT32(1, g_life.creates);
    TEST_ASSERT_EQUAL_UINT32(1, g_life.destroys);
    TEST_ASSERT_EQUAL_UINT32(5, g_life.configures);
    hdrg_context_destroy(ctx);

    free(pixels);
    free(rows);
}

/* a context with a filter of the asynchronous plugin on the PREFILTER stage */
/* the asynchronous plugin's filter id onto ctx's PREFILTER: its mode and depth (NULL: 1) */
static hdrg_context_t *async_context(const char *id, const char *mode, const char *depth,
                                     uint32_t threads)
{
    hdrg_options_t *options;
    hdrg_context_t *ctx;

    memset(&g_async, 0, sizeof(g_async));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, threads));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, async_init));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "mode", mode));

    if (depth != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "depth", depth));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, id, options));
    hdrg_options_destroy(options);

    return ctx;
}

/* pixels (TP_W x TP_H RGB8) as a still through ctx, which is then destroyed */
static hdrg_status_t async_still(hdrg_context_t *ctx, const uint8_t *pixels, uint8_t **output,
                                 size_t *size)
{
    hdrg_raw_image_t raw;
    hdrg_status_t status;

    memset(&raw, 0, sizeof(raw));
    raw.pixels = pixels;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    *output = NULL;

    status = hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, output, size);

    hdrg_context_destroy(ctx);

    return status;
}

/* TP_FRAMES frames through a session on ctx (then destroyed) into rows */
static void async_session(hdrg_context_t *ctx, const uint8_t *frames, uint8_t *rows)
{
    const size_t frame_size = (size_t)TP_W * TP_H * 3u;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, received;

    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    hdrg_session_config_init(&config);
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));

    memset(&in, 0, sizeof(in));
    in.strides[0] = (size_t)TP_W * 3u;
    memset(&out, 0, sizeof(out));
    out.strides[0] = (size_t)TP_W * 3u;
    sent = 0;
    received = 0;

    while (received < TP_FRAMES) {
        out.planes[0] = rows + received * frame_size;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            in.planes[0] = frames + sent * frame_size;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TP_FRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        received++;
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* TP_FRAMES frames of TP_W x TP_H RGB8, each a little different */
static uint8_t *async_frames(void)
{
    uint8_t *frames;
    size_t i;

    frames = (uint8_t *)malloc((size_t)TP_W * TP_H * 3u * TP_FRAMES);
    TEST_ASSERT_NOT_NULL(frames);

    for (i = 0; i < (size_t)TP_W * TP_H * 3u * TP_FRAMES; i++) {
        frames[i] = (uint8_t)((i * 7u + i / ((size_t)TP_W * TP_H * 3u) * 40u) % 251u);
    }

    return frames;
}

/*
 * An ASYNC filter's job ends with job_done (once; a second call is
 * refused, and so is job_done on a process job); its frames are the same
 * as those of the filter doing the same work in process; a failure given
 * to job_done or returned by submit fails the conversion; max_inflight is
 * 1 to 8, set in create alone, and a filter not setting it is refused
 */
static void test_async(void)
{
    uint8_t *frames, *ref, *output;
    size_t ref_size, size;

    frames = async_frames();

    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(async_context("org.test.async.cpu", "0", NULL, 0),
                                               frames, &ref, &ref_size));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_async.not_async);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(async_context("org.test.async.gpu", "0", NULL, 0),
                                               frames, &output, &size));
    TEST_ASSERT_EQUAL_size_t(ref_size, size);
    TEST_ASSERT_EQUAL_MEMORY(ref, output, size);
    TEST_ASSERT_EQUAL_UINT32(1, g_async.submits);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE, g_async.again);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_async.inflight[0]);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_async.inflight[1]);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE, g_async.inflight[2]);
    hdrg_free(output);

    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_PLUGIN,
        async_still(async_context("org.test.async.gpu", "2", NULL, 0), frames, &output, &size));
    TEST_ASSERT_NULL(output);
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_PLUGIN,
        async_still(async_context("org.test.async.gpu", "3", NULL, 0), frames, &output, &size));
    TEST_ASSERT_NULL(output);
    TEST_ASSERT_EQUAL_INT(
        HDRG_ERR_PLUGIN,
        async_still(async_context("org.test.async.gpu", "0", "0", 0), frames, &output, &size));
    TEST_ASSERT_NULL(output);

    hdrg_free(ref);
    free(frames);
}

/* jobs ended from another thread, in stills (any thread count) and in a session */
static void test_async_thread(void)
{
    const size_t frame_size = (size_t)TP_W * TP_H * 3u;
    uint32_t threads;
    uint8_t *frames, *ref, *output, *ref_rows, *rows;
    size_t ref_size, size;

    HDRG_TEST_REQUIRE_THREADS();

    frames = async_frames();
    ref_rows = (uint8_t *)malloc(frame_size * TP_FRAMES);
    rows = (uint8_t *)malloc(frame_size * TP_FRAMES);
    TEST_ASSERT_TRUE(ref_rows != NULL && rows != NULL);

    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(async_context("org.test.async.cpu", "0", NULL, 1),
                                               frames, &ref, &ref_size));

    for (threads = 1; threads <= 4; threads += 3) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              async_still(async_context("org.test.async.gpu", "1", NULL, threads),
                                          frames, &output, &size));
        TEST_ASSERT_EQUAL_size_t(ref_size, size);
        TEST_ASSERT_EQUAL_MEMORY(ref, output, size);
        hdrg_free(output);
    }

    async_session(async_context("org.test.async.cpu", "0", NULL, 0), frames, ref_rows);
    async_session(async_context("org.test.async.gpu", "1", NULL, 0), frames, rows);
    TEST_ASSERT_EQUAL_UINT32(TP_FRAMES, g_async.submits);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, frame_size * TP_FRAMES);

    hdrg_free(ref);
    free(frames);
    free(ref_rows);
    free(rows);
}

/* a context with the time window and asynchronous plugins, no filter added yet */
static hdrg_context_t *window_context(void)
{
    hdrg_context_t *ctx;

    memset(&g_windows, 0, sizeof(g_windows));
    memset(&g_async, 0, sizeof(g_async));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, window_init));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, async_init));

    return ctx;
}

/* the filter id onto ctx's stage: a time window's past / future and clip (NULL: none) */
static void window_add_at(hdrg_context_t *ctx, uint32_t stage, const char *id, const char *past,
                          const char *future, const char *clip)
{
    hdrg_options_t *options;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));

    if (past != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "past", past));
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "future", future));
    }

    if (clip != NULL) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "clip", clip));
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, stage, id, options));
    hdrg_options_destroy(options);
}

/* ... onto the PREFILTER chain, without a clip */
static void window_add(hdrg_context_t *ctx, const char *id, const char *past, const char *future)
{
    window_add_at(ctx, HDRG_STAGE_PREFILTER, id, past, future, NULL);
}

/*
 * The frames from start (TP_WFRAMES in all, RGB of depth bits) through a
 * session of config on ctx, the first preroll for its state alone: the rows
 * received after them, their number; *latency and *preroll as it reports,
 * and its latency report after the last frame (report NULL: none)
 */
static uint32_t window_report(hdrg_context_t *ctx, hdrg_session_config_t config,
                              const uint8_t *frames, uint8_t depth, uint32_t start,
                              uint32_t preroll, uint8_t *rows, uint32_t *latency,
                              uint32_t *reported, hdrg_session_latency_t *report)
{
    const size_t in_bytes = (size_t)TP_W * TP_H * 3u * (depth / 8u),
                 out_bytes = (size_t)TP_W * TP_H * 3u;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, received;

    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = depth;
    raw.stride = (size_t)TP_W * 3u * (depth / 8u);
    config.start = start;
    config.preroll = preroll;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));
    *latency = hdrg_session_latency(session);
    *reported = hdrg_session_preroll(session);

    memset(&in, 0, sizeof(in));
    in.strides[0] = raw.stride;
    memset(&out, 0, sizeof(out));
    out.strides[0] = (size_t)TP_W * 3u;
    sent = start;
    received = 0;

    for (;;) {
        out.planes[0] = rows + (size_t)received * out_bytes;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            in.planes[0] = frames + (size_t)sent * in_bytes;
            in.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TP_WFRAMES) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        TEST_ASSERT_TRUE(out.pts == (int64_t)(start + preroll + received));
        received++;
    }

    if (report != NULL) {
        report->struct_size = (uint32_t)sizeof(*report);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_latency_report(session, report));
    }

    hdrg_session_destroy(session);

    return received;
}

/* ... without the report */
static uint32_t window_session(hdrg_context_t *ctx, hdrg_session_config_t config,
                               const uint8_t *frames, uint8_t depth, uint32_t start,
                               uint32_t preroll, uint8_t *rows, uint32_t *latency,
                               uint32_t *reported)
{
    return window_report(ctx, config, frames, depth, start, preroll, rows, latency, reported, NULL);
}

/*
 * TP_WFRAMES frames of 8-bit Y'CbCr 4:4:4 (each three planes of TP_W x TP_H
 * from frames) through a session of ctx into rows (8-bit PQ R'G'B'); the
 * frames received
 */
static uint32_t yuv_session(hdrg_context_t *ctx, const uint8_t *frames, uint8_t *rows)
{
    const size_t plane = (size_t)TP_W * TP_H;
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent, received;
    uint8_t p;

    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    hdrg_session_config_init(&config);
    config.input.layout = HDRG_YUV_444;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 8;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    out.strides[0] = raw.stride;
    sent = 0;
    received = 0;

    for (;;) {
        out.planes[0] = rows + (size_t)received * plane * 3u;

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            for (p = 0; p < 3u; p++) {
                in.planes[p] = frames + ((size_t)sent * 3u + p) * plane;
                in.strides[p] = TP_W;
            }

            in.pts = (int64_t)sent;
            TEST_ASSERT_EQUAL_INT(
                HDRG_OK, hdrg_session_send_frame(session, (sent < TP_WFRAMES) ? &in : NULL));
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

    return received;
}

/*
 * the time window plugin's mean of past / future (clip > 0: of the frame's
 * clip, clips from frame 0, past frames of overlap) over TP_WFRAMES RGB16
 * frames, in place
 */
static void window_mean(uint16_t *frames, uint32_t past, uint32_t future, uint32_t clip)
{
    const size_t samples = (size_t)TP_W * TP_H * 3u;
    uint32_t t, f, lo, hi, sum;
    uint16_t *copy;
    size_t i;

    copy = (uint16_t *)malloc(samples * TP_WFRAMES * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, frames, samples * TP_WFRAMES * sizeof(uint16_t));

    for (t = 0; t < TP_WFRAMES; t++) {
        lo = t - HDRG_MIN(past, t);
        hi = t + HDRG_MIN(future, TP_WFRAMES - 1u - t);

        /* a clip's (past: its overlap) */
        if (clip > 0) {
            lo = t - t % clip;
            lo = lo - HDRG_MIN(past, lo);
            hi = HDRG_MIN(t - t % clip + clip - 1u + past, TP_WFRAMES - 1u);
        }

        for (i = 0; i < samples; i++) {
            sum = 0;

            for (f = lo; f <= hi; f++) {
                sum += copy[(size_t)f * samples + i];
            }

            frames[(size_t)t * samples + i] =
                (uint16_t)((sum + (hi - lo + 1u) / 2u) / (hi - lo + 1u));
        }
    }

    free(copy);
}

/*
 * A window across a scene cut (frame 5: another picture): a SCALE job's
 * shot ends at it (job_shot); a PREFILTER job's frames come before the
 * cuts are judged (HDRG_ERR_NOT_FOUND); a still's shot is its own frame
 */
static void test_window_shots(void)
{
    static const uint8_t shots[TP_WFRAMES] = {0x02, 0x12, 0x22, 0x21, 0x20, 0x02, 0x11, 0x20};
    const size_t samples = (size_t)TP_W * TP_H * 3u;
    hdrg_session_config_t config;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    uint32_t i, latency, preroll;
    uint8_t *frames, *rows, *output;
    size_t size;

    frames = (uint8_t *)malloc(samples * TP_WFRAMES);
    rows = (uint8_t *)malloc(samples * TP_WFRAMES);
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL);

    for (i = 0; i < samples * TP_WFRAMES; i++) {
        frames[i] =
            (i / samples < 5u) ? (uint8_t)(30u + (i % 97u) / 4u) : (uint8_t)(180u + (i * 7u % 61u));
    }

    hdrg_session_config_init(&config);
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.frame", "2", "2", NULL);
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, g_windows.jobs);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, g_windows.shot);

    for (i = 0; i < TP_WFRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT32(i, g_windows.index[i]);
        TEST_ASSERT_EQUAL_HEX8(shots[i], g_windows.shots[i]);
    }

    ctx = window_context();
    window_add(ctx, "org.test.window.frame", "2", "2");
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, g_windows.shot);

    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.frame", "2", "2", NULL);
    memset(&raw, 0, sizeof(raw));
    raw.pixels = frames;
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &output, &size));
    hdrg_free(output);
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, g_windows.shot);
    TEST_ASSERT_EQUAL_HEX8(0x00, g_windows.shots[0]);

    free(frames);
    free(rows);
}

/*
 * A filter's time window. A still's job has its own frame alone. A
 * session's frame t is the mean of the frames t - past .. t + future it has
 * (fewer at the ends), delayed by future (the latency) with past more frames
 * of preroll; a chain of windows and filters between them composes, and the
 * frames it holds at the end come out. A session started the preroll before
 * a frame gives it as the whole run does. filter_set_window is create's
 * alone, 8 frames at most; frames beyond the window are not found.
 */
static void test_window(void)
{
    static const uint32_t seeks[3] = {1, 3, 6};
    static const uint8_t seen[TP_WFRAMES] = {0x01, 0x11, 0x21, 0x21, 0x21, 0x21, 0x21, 0x20};
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    hdrg_session_config_t config, fixed;
    hdrg_context_t *ctx;
    uint32_t i, latency, preroll, ref_latency, ref_preroll, got, start;
    uint16_t *expect;
    uint8_t *frames, *ref, *output, *rows, *ref_rows, *seek;
    size_t ref_size, size;

    frames = (uint8_t *)malloc(bytes);
    expect = (uint16_t *)malloc(bytes * sizeof(uint16_t));
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    seek = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && expect != NULL && rows != NULL && ref_rows != NULL &&
                     seek != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
        expect[i] = (uint16_t)(frames[i] * 257u);
    }

    /* a still: its own frame alone */
    ctx = window_context();
    window_add(ctx, "org.test.window.frame", "0", "0");
    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &ref, &ref_size));
    ctx = window_context();
    window_add(ctx, "org.test.window.frame", "2", "1");
    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &output, &size));
    TEST_ASSERT_EQUAL_size_t(ref_size, size);
    TEST_ASSERT_EQUAL_MEMORY(ref, output, size);
    TEST_ASSERT_EQUAL_UINT32(1, g_windows.jobs);
    TEST_ASSERT_EQUAL_UINT8(0, g_windows.seen[0]);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_windows.bad);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_STATE, g_windows.late);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, g_windows.beyond);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_windows.plane_in);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_windows.plane_out);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, g_windows.plane_frame);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, g_windows.handle_beyond);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_NOT_FOUND, g_windows.device_beyond);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, g_windows.side_query);
    TEST_ASSERT_EQUAL_size_t(HDRG_SD_NOISE_FLOATS * sizeof(float), g_windows.side_needed);
    hdrg_free(ref);
    hdrg_free(output);

    /* a session: the mean over the window, against its frames given at 16 bits */
    hdrg_session_config_init(&config);
    ctx = window_context();
    window_add(ctx, "org.test.window.frame", "2", "1");
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, g_windows.jobs);
    TEST_ASSERT_EQUAL_MEMORY(seen, g_windows.seen, TP_WFRAMES);
    hdrg_context_destroy(ctx);

    window_mean(expect, 2, 1, 0);
    ctx = window_context();
    window_add(ctx, "org.test.window.bands", "0", "0");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, config, (const uint8_t *)(const void *)expect, 16,
                                            0, 0, ref_rows, &ref_latency, &ref_preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    TEST_ASSERT_EQUAL_UINT32(ref_latency + 1u, latency);
    TEST_ASSERT_EQUAL_UINT32(ref_preroll + 2u, preroll);

    /* a chain: a window, a filter, a window in bands; STATIC without the limiter (no preroll) */
    hdrg_session_config_init(&fixed);
    fixed.temporal = HDRG_TEMPORAL_STATIC;
    fixed.past = 0;
    fixed.flash_off = true;

    for (i = 0; i < bytes; i++) {
        expect[i] = (uint16_t)(frames[i] * 257u);
    }

    window_mean(expect, 1, 1, 0);

    for (i = 0; i < bytes; i++) {
        expect[i] = (uint16_t)(65535u - expect[i]);
    }

    window_mean(expect, 0, 2, 0);
    ctx = window_context();
    window_add(ctx, "org.test.window.frame", "1", "1");
    window_add(ctx, "org.test.async.cpu", NULL, NULL);
    window_add(ctx, "org.test.window.bands", "0", "2");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, fixed, frames, 8, 0, 0, rows, &latency, &preroll));
    TEST_ASSERT_EQUAL_UINT32(3, latency);
    TEST_ASSERT_EQUAL_UINT32(1, preroll);

    /* seeking: from the preroll before frame n */
    for (i = 0; i < 3u; i++) {
        start = seeks[i] - HDRG_MIN(seeks[i], preroll);
        got = window_session(ctx, fixed, frames, 8, start, seeks[i] - start, seek, &latency,
                             &ref_preroll);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES - seeks[i], got);
        TEST_ASSERT_EQUAL_MEMORY(rows + (size_t)seeks[i] * samples, seek, (size_t)got * samples);
    }

    hdrg_context_destroy(ctx);

    ctx = window_context();
    window_add(ctx, "org.test.window.bands", "0", "0");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, fixed, (const uint8_t *)(const void *)expect, 16,
                                            0, 0, ref_rows, &ref_latency, &ref_preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);

    free(frames);
    free(expect);
    free(rows);
    free(ref_rows);
    free(seek);
}

/* the asynchronous filter onto ctx's stage: its mode and depth (max_inflight) */
static void inflight_add(hdrg_context_t *ctx, uint32_t stage, const char *mode, const char *depth)
{
    hdrg_options_t *options;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "mode", mode));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "depth", depth));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_context_add_filter(ctx, stage, "org.test.async.gpu", options));
    hdrg_options_destroy(options);
}

/*
 * A session's frames in flight: an ASYNC filter of max_inflight m
 * has m frames submitted before the earliest lands (the latency m - 1) and
 * gives the frames of the filter doing the same in process: alone, before a
 * time window and after one, its jobs ended within submit or from threads of
 * its own. Two in a row hand the frames over: each job of the second reads
 * a frame the first wrote (no copy). Y'CbCr frames (converted straight into
 * the first one's ring) give the same. A job ending in a failure fails the
 * send that lands it; a session destroyed with frames in flight waits for
 * them.
 */
static void test_inflight(void)
{
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    static const char *const modes[2] = {"0", "1"};
    hdrg_session_config_t config;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t i, m, latency, preroll, ref_latency, handed, j, k;
    uint8_t *frames, *rows, *ref_rows;

    frames = (uint8_t *)malloc(bytes);
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL && ref_rows != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
    }

    hdrg_session_config_init(&config);
    config.future = 0; /* the filters' delays alone */

    for (m = 0; m < 1u + TP_THREADS; m++) {
        /* alone */
        ctx = window_context();
        window_add(ctx, "org.test.async.cpu", NULL, NULL);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows,
                                                            &ref_latency, &preroll));
        hdrg_context_destroy(ctx);

        ctx = window_context();
        inflight_add(ctx, HDRG_STAGE_PREFILTER, modes[m], "3");
        TEST_ASSERT_EQUAL_UINT32(
            TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, g_async.submits);
        hdrg_context_destroy(ctx);
        TEST_ASSERT_EQUAL_UINT32(ref_latency + 2u, latency);
        TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);

        /* before a time window */
        ctx = window_context();
        window_add(ctx, "org.test.async.cpu", NULL, NULL);
        window_add(ctx, "org.test.window.bands", "1", "1");
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows,
                                                            &ref_latency, &preroll));
        hdrg_context_destroy(ctx);

        ctx = window_context();
        inflight_add(ctx, HDRG_STAGE_PREFILTER, modes[m], "2");
        window_add(ctx, "org.test.window.bands", "1", "1");
        TEST_ASSERT_EQUAL_UINT32(
            TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
        hdrg_context_destroy(ctx);
        TEST_ASSERT_EQUAL_UINT32(ref_latency + 1u, latency);
        TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);

        /* after one, the chain's last */
        ctx = window_context();
        window_add(ctx, "org.test.window.frame", "1", "0");
        window_add(ctx, "org.test.async.cpu", NULL, NULL);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows,
                                                            &ref_latency, &preroll));
        hdrg_context_destroy(ctx);

        ctx = window_context();
        window_add(ctx, "org.test.window.frame", "1", "0");
        inflight_add(ctx, HDRG_STAGE_PREFILTER, modes[m], "4");
        TEST_ASSERT_EQUAL_UINT32(
            TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
        hdrg_context_destroy(ctx);
        TEST_ASSERT_EQUAL_UINT32(ref_latency + 3u, latency);
        TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);

        /* two in a row: the second's every input one of the first's outputs itself */
        ctx = window_context();
        window_add(ctx, "org.test.async.cpu", NULL, NULL);
        window_add(ctx, "org.test.async.cpu", NULL, NULL);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows,
                                                            &ref_latency, &preroll));
        hdrg_context_destroy(ctx);

        ctx = window_context();
        inflight_add(ctx, HDRG_STAGE_PREFILTER, modes[m], "2");
        inflight_add(ctx, HDRG_STAGE_PREFILTER, modes[m], "3");
        TEST_ASSERT_EQUAL_UINT32(
            TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
        TEST_ASSERT_EQUAL_UINT32(2u * TP_WFRAMES, g_async.submits);
        handed = 0;

        for (j = 0; j < 2u * TP_WFRAMES; j++) {
            for (k = 0; k < j && g_async.ins[j] != g_async.outs[k]; k++) {
            }

            handed += (k < j) ? 1u : 0u;
        }

        hdrg_context_destroy(ctx);
        TEST_ASSERT_EQUAL_UINT32(ref_latency + 3u, latency);
        TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, handed);

        /* Y'CbCr: converted straight into the first one's ring, the frames as in process */
        ctx = window_context();
        window_add(ctx, "org.test.async.cpu", NULL, NULL);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, yuv_session(ctx, frames, ref_rows));
        hdrg_context_destroy(ctx);

        ctx = window_context();
        inflight_add(ctx, HDRG_STAGE_PREFILTER, modes[m], "2");
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, yuv_session(ctx, frames, rows));
        hdrg_context_destroy(ctx);
        TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    }

    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    memset(&in, 0, sizeof(in));
    in.strides[0] = raw.stride;
    memset(&out, 0, sizeof(out));
    out.planes[0] = rows;
    out.strides[0] = raw.stride;

    /* the earliest lands once three are in flight; destroyed with two of them */
    ctx = window_context();
    inflight_add(ctx, HDRG_STAGE_PREFILTER, modes[TP_THREADS], "3");
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));

    for (i = 0; i < 3u; i++) {
        in.planes[0] = frames + (size_t)i * samples;
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
        TEST_ASSERT_EQUAL_INT((i < 2u) ? HDRG_ERR_AGAIN : HDRG_OK,
                              hdrg_session_receive_frame(session, &out));
    }

    TEST_ASSERT_EQUAL_UINT32(3, g_async.submits);
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    /* a failure lands with the third frame */
    ctx = window_context();
    inflight_add(ctx, HDRG_STAGE_PREFILTER, "2", "3");
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));

    for (i = 0; i < 3u; i++) {
        in.planes[0] = frames + (size_t)i * samples;
        TEST_ASSERT_EQUAL_INT((i < 2u) ? HDRG_OK : HDRG_ERR_PLUGIN,
                              hdrg_session_send_frame(session, &in));
    }

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    free(frames);
    free(rows);
    free(ref_rows);
}

/*
 * Clips and SCALE windows: a clip's job reads its frame's clip (clips from
 * the stream's first frame, the job's index in the stream), delayed by the
 * clip - 1 and with as much preroll; PREFILTER windows stop at 8 frames,
 * SCALE ones at 32; a session's SCALE with a window or a clip gives the
 * means of its frames (the analysis stays on the frames as sent: STATIC
 * here), seeks to the frames of the whole run, and an ASYNC SCALE in flight
 * gives the frames of the filter doing the same in process
 */
static void test_clip_and_scale_window(void)
{
    static const uint32_t seeks[3] = {2, 5, 7};
    static const uint8_t clip3[TP_WFRAMES] = {0x02, 0x11, 0x20, 0x02, 0x11, 0x20, 0x01, 0x10},
                         clip4[TP_WFRAMES] = {0x03, 0x12, 0x21, 0x30, 0x03, 0x12, 0x21, 0x30};
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    static const char *const modes[2] = {"0", "1"};
    hdrg_session_config_t config, fixed;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t i, m, latency, preroll, ref_latency, ref_preroll, start, got;
    uint16_t *expect;
    uint8_t *frames, *rows, *ref_rows, *seek, *output;
    size_t size;

    frames = (uint8_t *)malloc(bytes);
    expect = (uint16_t *)malloc(bytes * sizeof(uint16_t));
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    seek = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && expect != NULL && rows != NULL && ref_rows != NULL &&
                     seek != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
    }

    hdrg_session_config_init(&config);
    config.future = 0; /* the filters' delays alone */
    hdrg_session_config_init(&fixed);
    fixed.temporal = HDRG_TEMPORAL_STATIC;
    fixed.past = 0;
    fixed.flash_off = true;
    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;

    /* the windows' limits: 8 frames in PREFILTER, 32 in SCALE */
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_PREFILTER, "org.test.window.bands", "9", "0", NULL);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));
    hdrg_context_destroy(ctx);
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.bands", "32", "32", NULL);
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &fixed, &session));
    TEST_ASSERT_EQUAL_UINT32(32, hdrg_session_latency(session));
    TEST_ASSERT_EQUAL_UINT32(32, hdrg_session_preroll(session));
    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);

    /* a still: a clip's job has its own frame alone */
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_PREFILTER, "org.test.window.frame", NULL, NULL, "3");
    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &output, &size));
    TEST_ASSERT_EQUAL_UINT32(1, g_windows.jobs);
    TEST_ASSERT_EQUAL_UINT8(0, g_windows.seen[0]);
    TEST_ASSERT_EQUAL_UINT32(0, g_windows.index[0]);
    hdrg_free(output);

    /* a PREFILTER clip of 3 */
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_PREFILTER, "org.test.window.frame", NULL, NULL, "3");
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, g_windows.jobs);
    TEST_ASSERT_EQUAL_MEMORY(clip3, g_windows.seen, TP_WFRAMES);

    for (i = 0; i < TP_WFRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT32(i, g_windows.index[i]);
    }

    hdrg_context_destroy(ctx);

    for (i = 0; i < bytes; i++) {
        expect[i] = (uint16_t)(frames[i] * 257u);
    }

    window_mean(expect, 0, 0, 3);
    ctx = window_context();
    window_add(ctx, "org.test.window.bands", "0", "0");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, config, (const uint8_t *)(const void *)expect, 16,
                                            0, 0, ref_rows, &ref_latency, &ref_preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    TEST_ASSERT_EQUAL_UINT32(ref_latency + 2u, latency);
    TEST_ASSERT_EQUAL_UINT32(ref_preroll + 2u, preroll);

    /* a SCALE window, in bands */
    for (i = 0; i < bytes; i++) {
        expect[i] = (uint16_t)(frames[i] * 257u);
    }

    window_mean(expect, 2, 1, 0);
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.bands", "2", "1", NULL);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, fixed, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, latency);
    TEST_ASSERT_EQUAL_UINT32(2, preroll);
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.bands", "0", "0", NULL);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, fixed, (const uint8_t *)(const void *)expect, 16,
                                            0, 0, ref_rows, &ref_latency, &ref_preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);

    /* a SCALE clip of 4 on whole frames, and seeking with it */
    for (i = 0; i < bytes; i++) {
        expect[i] = (uint16_t)(frames[i] * 257u);
    }

    window_mean(expect, 0, 0, 4);
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.frame", NULL, NULL, "4");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, fixed, frames, 8, 0, 0, rows, &latency, &preroll));
    TEST_ASSERT_EQUAL_MEMORY(clip4, g_windows.seen, TP_WFRAMES);
    TEST_ASSERT_EQUAL_UINT32(3, latency);
    TEST_ASSERT_EQUAL_UINT32(3, preroll);

    for (i = 0; i < 3u; i++) {
        start = seeks[i] - HDRG_MIN(seeks[i], preroll);
        got = window_session(ctx, fixed, frames, 8, start, seeks[i] - start, seek, &latency,
                             &ref_preroll);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES - seeks[i], got);
        TEST_ASSERT_EQUAL_MEMORY(rows + (size_t)seeks[i] * samples, seek, (size_t)got * samples);
    }

    hdrg_context_destroy(ctx);
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.bands", "0", "0", NULL);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, fixed, (const uint8_t *)(const void *)expect, 16,
                                            0, 0, ref_rows, &ref_latency, &ref_preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);

    /* an ASYNC SCALE in flight, its jobs ended within submit or from threads */
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.async.cpu", NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows,
                                                        &ref_latency, &ref_preroll));
    hdrg_context_destroy(ctx);

    for (m = 0; m < 1u + TP_THREADS; m++) {
        ctx = window_context();
        inflight_add(ctx, HDRG_STAGE_SCALE, modes[m], "3");
        TEST_ASSERT_EQUAL_UINT32(
            TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, g_async.submits);
        hdrg_context_destroy(ctx);
        TEST_ASSERT_EQUAL_UINT32(ref_latency + 2u, latency);
        TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    }

    free(frames);
    free(expect);
    free(rows);
    free(ref_rows);
    free(seek);
}

/*
 * The latency profiles: the frames in flight of an ASYNC filter (PREFILTER
 * or SCALE) delay each frame under PLAYBACK and OFFLINE, LOW waiting for
 * each job at once and giving the same frames; a filter's time window
 * delays it under all; LOW takes no window future; OFFLINE adds the lights'
 * long window (24 frames). The report gives the
 * delay by its causes, under each profile and at the rate, and the time the
 * calls took
 */
static void test_latency_profiles(void)
{
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    hdrg_session_config_t config, ahead, realtime, offline;
    hdrg_session_latency_t report, live;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t i, latency, preroll, width, height;
    uint8_t *frames, *rows, *ref_rows;

    frames = (uint8_t *)malloc(bytes);
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL && ref_rows != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
    }

    hdrg_session_config_init(&config);
    config.future = 0; /* the filters' delays alone */
    config.rate_num = 60;
    config.rate_den = 1;
    ahead = config;
    ahead.future = 2;
    realtime = config;
    realtime.latency = HDRG_LATENCY_LOW;
    offline = config;
    offline.latency = HDRG_LATENCY_OFFLINE;

    /* PREFILTER: three in flight before a window of one frame each way */
    ctx = window_context();
    inflight_add(ctx, HDRG_STAGE_PREFILTER, "0", "3");
    window_add(ctx, "org.test.window.bands", "1", "1");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, window_report(ctx, config, frames, 8, 0, 0, ref_rows,
                                                       &latency, &preroll, &report));
    TEST_ASSERT_EQUAL_UINT32(3, latency);
    TEST_ASSERT_EQUAL_UINT32(3, report.frames);
    TEST_ASSERT_EQUAL_UINT32(0, report.window);
    TEST_ASSERT_EQUAL_UINT32(1, report.prefilter);
    TEST_ASSERT_EQUAL_UINT32(2, report.prefilter_flight);
    TEST_ASSERT_EQUAL_UINT32(0, report.scale + report.scale_flight);
    TEST_ASSERT_EQUAL_UINT32(3, report.profiles[HDRG_LATENCY_PLAYBACK]);
    TEST_ASSERT_EQUAL_UINT32(1, report.profiles[HDRG_LATENCY_LOW]);
    TEST_ASSERT_EQUAL_UINT32(HDRG_BUILTIN_STATS ? 3 + 24 : 3,
                             report.profiles[HDRG_LATENCY_OFFLINE]); /* the lights' window */
    TEST_ASSERT_EQUAL_UINT32(report.frames, report.profiles[report.profile]);
    TEST_ASSERT_EQUAL_UINT32(preroll, report.preroll);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, report.sent);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, report.received);
    TEST_ASSERT_EQUAL_UINT8(HDRG_LATENCY_PLAYBACK, report.profile);
    HDRG_TEST_NEAR(1e-3, 60.0, report.rate);
    HDRG_TEST_NEAR(1e-3, 50.0, report.frames_ms);
    TEST_ASSERT_TRUE(report.work_ms > 0.0f && report.work_max_ms > 0.0f);
    TEST_ASSERT_TRUE(report.work_max_ms <= report.work_ms * (float)(2u * TP_WFRAMES));
    HDRG_TEST_NEAR(1e-3, report.frames_ms + report.work_ms, report.total_ms);

    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_report(ctx, realtime, frames, 8, 0, 0, rows, &latency, &preroll, &live));
    TEST_ASSERT_EQUAL_UINT32(1, latency);
    TEST_ASSERT_EQUAL_UINT32(1, live.prefilter);
    TEST_ASSERT_EQUAL_UINT32(0, live.prefilter_flight);
    TEST_ASSERT_EQUAL_UINT32(3, live.profiles[HDRG_LATENCY_PLAYBACK]);
    TEST_ASSERT_EQUAL_UINT32(1, live.profiles[HDRG_LATENCY_LOW]);
    TEST_ASSERT_EQUAL_UINT8(HDRG_LATENCY_LOW, live.profile);
    TEST_ASSERT_EQUAL_UINT32(live.frames, live.profiles[live.profile]);
    HDRG_TEST_NEAR(1e-3, 1000.0 / 60.0, live.frames_ms);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);

    /* OFFLINE: the frames its own profile counts (the lights' long window) */
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_report(ctx, offline, frames, 8, 0, 0, rows, &latency, &preroll, &live));
    TEST_ASSERT_EQUAL_UINT32(HDRG_BUILTIN_STATS ? 3 + 24 : 3, latency);
    TEST_ASSERT_EQUAL_UINT32(live.frames, live.profiles[HDRG_LATENCY_OFFLINE]);
    TEST_ASSERT_EQUAL_UINT32(report.profiles[HDRG_LATENCY_LOW], live.profiles[HDRG_LATENCY_LOW]);

#if HDRG_BUILTIN_STATS
    /* the statistics window's future after the chain's (no window in a build without it) */
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_report(ctx, ahead, frames, 8, 0, 0, rows, &latency, &preroll, &report));
    TEST_ASSERT_EQUAL_UINT32(5, report.frames);
    TEST_ASSERT_EQUAL_UINT32(2, report.window);
    TEST_ASSERT_EQUAL_UINT32(5, report.profiles[HDRG_LATENCY_PLAYBACK]);
    TEST_ASSERT_EQUAL_UINT32(1, report.profiles[HDRG_LATENCY_LOW]);
    TEST_ASSERT_EQUAL_UINT32(report.frames, report.profiles[report.profile]);
#endif

    /* LOW takes no window future; the profile is one of the three */
    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    ahead.latency = HDRG_LATENCY_LOW;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &ahead, &session));
    ahead.future = 0;
    ahead.latency = HDRG_LATENCY_PROFILES;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &ahead, &session));

    /* before a frame: nothing measured; a report of another size is refused */
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));
    report.struct_size = (uint32_t)sizeof(report) - 1u;
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_session_latency_report(session, &report));
    report.struct_size = (uint32_t)sizeof(report);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_latency_report(session, &report));
    TEST_ASSERT_EQUAL_UINT32(0, report.sent + report.received);
    TEST_ASSERT_TRUE(report.work_ms == 0.0f && report.work_max_ms == 0.0f);
    TEST_ASSERT_EQUAL_UINT32(3, report.frames);
    hdrg_session_destroy(session);

    /* a NULL session: nothing (the size written as 0, its outputs may be NULL too) */
    width = 1;
    height = 1;
    TEST_ASSERT_EQUAL_UINT32(0, hdrg_session_latency(NULL));
    TEST_ASSERT_EQUAL_UINT32(0, hdrg_session_preroll(NULL));
    TEST_ASSERT_TRUE(hdrg_session_peak(NULL) == 0.0f);
    hdrg_session_size(NULL, &width, &height);
    TEST_ASSERT_EQUAL_UINT32(0, width + height);
    hdrg_session_size(NULL, NULL, NULL);
    hdrg_context_destroy(ctx);

    /* SCALE: two in flight */
    ctx = window_context();
    inflight_add(ctx, HDRG_STAGE_SCALE, "0", "2");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, window_report(ctx, config, frames, 8, 0, 0, ref_rows,
                                                       &latency, &preroll, &report));
    TEST_ASSERT_EQUAL_UINT32(1, latency);
    TEST_ASSERT_EQUAL_UINT32(0, report.scale);
    TEST_ASSERT_EQUAL_UINT32(1, report.scale_flight);
    TEST_ASSERT_EQUAL_UINT32(0, report.profiles[HDRG_LATENCY_LOW]);
    TEST_ASSERT_EQUAL_UINT32(report.frames, report.profiles[report.profile]);

    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_report(ctx, realtime, frames, 8, 0, 0, rows, &latency, &preroll, &live));
    TEST_ASSERT_EQUAL_UINT32(0, latency);
    TEST_ASSERT_EQUAL_UINT32(0, live.scale_flight);
    TEST_ASSERT_EQUAL_UINT32(1, live.profiles[HDRG_LATENCY_PLAYBACK]);
    TEST_ASSERT_EQUAL_UINT32(live.frames, live.profiles[live.profile]);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    hdrg_context_destroy(ctx);

    free(frames);
    free(rows);
    free(ref_rows);
}

/*
 * count frames (all the same) through a session of config on ctx and its
 * report after them; *late: the frames received late
 */
static void late_run(hdrg_context_t *ctx, hdrg_session_config_t config, uint32_t count,
                     hdrg_session_latency_t *report, uint32_t *late, uint32_t *shed)
{
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_status_t status;
    uint32_t sent;
    uint8_t *frame, *row;

    frame = (uint8_t *)malloc((size_t)TP_W * TP_H * 3u);
    row = (uint8_t *)malloc((size_t)TP_W * TP_H * 3u);
    TEST_ASSERT_TRUE(frame != NULL && row != NULL);
    memcpy(frame, g_pixels, (size_t)TP_W * TP_H * 3u);

    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));

    memset(&in, 0, sizeof(in));
    in.planes[0] = frame;
    in.strides[0] = raw.stride;
    memset(&out, 0, sizeof(out));
    out.planes[0] = row;
    out.strides[0] = raw.stride;
    sent = 0;
    *late = 0;
    *shed = 0;

    for (;;) {
        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            TEST_ASSERT_EQUAL_INT(HDRG_OK,
                                  hdrg_session_send_frame(session, (sent < count) ? &in : NULL));
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        *late += out.late ? 1u : 0u;
        *shed += out.shed ? 1u : 0u;
    }

    report->struct_size = (uint32_t)sizeof(*report);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_latency_report(session, report));
    hdrg_session_destroy(session);
    free(frame);
    free(row);
}

/*
 * Keeping up with the rate: at a million frames a second every frame is
 * late, under LOW from the second one (the first sets up), under PLAYBACK
 * once the last second's 240 frames (the ring's most) are in, and never
 * under OFFLINE; a frame in 1000 seconds is never late. The report counts
 * them, gives the frame interval and the parts of the calls' time, and its
 * advice under LOW with an ASYNC filter that would keep three frames in
 * flight is PLAYBACK. With shed, the frames sent after the first late one
 * pass the PREFILTER filter through and say so; without it, none
 */
static void test_late(void)
{
    hdrg_session_config_t config;
    hdrg_session_latency_t report;
    hdrg_context_t *ctx;
    uint32_t late, shed;

    ctx = window_context();
    inflight_add(ctx, HDRG_STAGE_PREFILTER, "0", "3");
    hdrg_session_config_init(&config);
    config.rate_num = 1000000;
    config.rate_den = 1;

    config.latency = HDRG_LATENCY_LOW;
    late_run(ctx, config, 10, &report, &late, &shed);
    TEST_ASSERT_EQUAL_UINT32(9, late);
    TEST_ASSERT_EQUAL_UINT32(9, report.late_frames);
    TEST_ASSERT_TRUE(report.late);
    HDRG_TEST_NEAR(1e-6, 0.001, report.interval_ms);
    TEST_ASSERT_TRUE(report.prefilter_ms > 0.0f && report.analyze_ms > 0.0f &&
                     report.apply_ms > 0.0f);
    TEST_ASSERT_TRUE(report.convert_ms == 0.0f);
    TEST_ASSERT_TRUE(report.prefilter_ms + report.analyze_ms + report.apply_ms <=
                     report.work_ms * 1.001f);
    TEST_ASSERT_TRUE((report.advice & HDRG_ADVICE_LATENCY) != 0);
    TEST_ASSERT_EQUAL_UINT32(0, shed);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, report.shed_at);

    /* shed: from the frame sent after the second one received (the first late) */
    config.shed = true;
    late_run(ctx, config, 10, &report, &late, &shed);
    TEST_ASSERT_EQUAL_UINT32(9, late);
    TEST_ASSERT_EQUAL_UINT32(2, report.shed_at);
    TEST_ASSERT_EQUAL_UINT32(8, shed);
    config.shed = false;

    config.latency = HDRG_LATENCY_PLAYBACK;
    late_run(ctx, config, 250, &report, &late, &shed);
    TEST_ASSERT_TRUE(late >= 1u && late <= 11u);
    TEST_ASSERT_EQUAL_UINT32(late, report.late_frames);
    TEST_ASSERT_TRUE((report.advice & HDRG_ADVICE_LATENCY) == 0);

    config.latency = HDRG_LATENCY_OFFLINE;
    late_run(ctx, config, 250, &report, &late, &shed);
    TEST_ASSERT_EQUAL_UINT32(0, late + report.late_frames);
    TEST_ASSERT_FALSE(report.late);
    TEST_ASSERT_EQUAL_UINT32(0, report.advice);

    config.latency = HDRG_LATENCY_LOW;
    config.rate_num = 1;
    config.rate_den = 1000;
    late_run(ctx, config, 10, &report, &late, &shed);
    TEST_ASSERT_EQUAL_UINT32(0, late + report.late_frames);
    TEST_ASSERT_EQUAL_UINT32(0, report.advice);
    hdrg_context_destroy(ctx);
}

/* the format plugin's filter id onto ctx's PREFILTER chain: in = out = format, a past window */
static void format_add(hdrg_context_t *ctx, const char *id, uint32_t format, uint32_t past,
                       uint32_t fail)
{
    hdrg_options_t *options;
    char text[16];

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
    snprintf(text, sizeof(text), "%u", format);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "in", text));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "out", text));
    snprintf(text, sizeof(text), "%u", past);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "past", text));
    snprintf(text, sizeof(text), "%u", fail);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "fail", text));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, id, options));
    hdrg_options_destroy(options);
}

/* a context with the format and time window plugins */
static hdrg_context_t *format_context(void)
{
    hdrg_context_t *ctx;

    ctx = window_context();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, format_init));

    return ctx;
}

/*
 * Frame formats. The conversions: RGB16, RGBA16, planar RGB16 and single
 * floats exact (floats out of [0, 1] clamped), RGBA8 exact on 8-bit codes,
 * RGB10A2's bit layout, half floats within 16 codes, 4:2:0 of a flat color close; a whole frame's
 * strides multiples of 256. A filter copying its frames in each format (whole frames; bands but
 * 4:2:0, refused there) gives what the frames converted there and back give
 * as RGB16; a time window over frames of another format than RGB16 gives
 * the mean over RGB16 frames
 */
static void test_formats(void)
{
    static const uint32_t banded[7] = {
        HDRG_FORMAT_RGB16, HDRG_FORMAT_RGBA16,  HDRG_FORMAT_RGB16_PLANAR, HDRG_FORMAT_RGBF16_PLANAR,
        HDRG_FORMAT_RGBA8, HDRG_FORMAT_RGB10A2, HDRG_FORMAT_RGBF32_PLANAR};
    static const uint16_t codes[12] = {0,     65535, 32768, 257 * 7, 257 * 200, 1,
                                       65534, 12345, 257,   40000,   257 * 99,  300};
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    hdrg_format_layout_t layout;
    hdrg_format_frame_t view;
    hdrg_format_yuv_t yuv;
    hdrg_session_config_t config;
    hdrg_raw_image_t raw;
    hdrg_context_t *ctx;
    hdrg_exec_t exec;
    uint32_t i, f, word, latency, preroll;
    uint16_t back[12], *flat, *expect;
    uint8_t *frame, *frames, *output, *ref, *rows, *ref_rows;
    size_t size, ref_size;
    void *scratch;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
    hdrg_context_exec(ctx, &exec);
    yuv.matrix = 1;
    yuv.full_range = false;
    frame = (uint8_t *)malloc(1u << 16);
    TEST_ASSERT_NOT_NULL(frame);

    /* the conversions of a 4 x 1 frame */
    for (i = 0; i < 7u; i++) {
        TEST_ASSERT_TRUE(hdrg_format_layout(banded[i], 4, 1, true, &layout));
        TEST_ASSERT_EQUAL_size_t(0, layout.strides[0] % HDRG_FORMAT_ALIGN);
        TEST_ASSERT_EQUAL_size_t(0, layout.bytes % HDRG_FORMAT_PAGE);
        hdrg_format_view(&layout, frame, &view);
        hdrg_format_pack(&exec, &layout, &yuv, codes, sizeof(codes), 0, 1, &view, NULL);
        hdrg_format_unpack(&exec, &layout, &yuv, &view, 0, 1, back, sizeof(back), NULL);

        for (f = 0; f < 12u; f++) {
            if (banded[i] <= HDRG_FORMAT_RGB16_PLANAR || banded[i] == HDRG_FORMAT_RGBF32_PLANAR) {
                TEST_ASSERT_EQUAL_UINT16(codes[f], back[f]);
            } else if (banded[i] == HDRG_FORMAT_RGBF16_PLANAR) {
                TEST_ASSERT_TRUE(abs((int32_t)codes[f] - (int32_t)back[f]) <= 16);
            } else if (banded[i] == HDRG_FORMAT_RGBA8 && codes[f] % 257u == 0) {
                TEST_ASSERT_EQUAL_UINT16(codes[f], back[f]);
            }
        }
    }

    TEST_ASSERT_TRUE(hdrg_format_layout(HDRG_FORMAT_RGB10A2, 4, 1, true, &layout));
    hdrg_format_view(&layout, frame, &view);
    hdrg_format_pack(&exec, &layout, &yuv, codes, sizeof(codes), 0, 1, &view, NULL);
    memcpy(&word, frame, sizeof(word));
    TEST_ASSERT_EQUAL_HEX32(0u | (1023u << 10) | (512u << 20) | (3u << 30), word);
    TEST_ASSERT_TRUE(hdrg_format_layout(HDRG_FORMAT_RGBF16_PLANAR, 4, 1, true, &layout));
    hdrg_format_view(&layout, frame, &view);
    hdrg_format_pack(&exec, &layout, &yuv, codes, sizeof(codes), 0, 1, &view, NULL);
    TEST_ASSERT_EQUAL_HEX16(0x0000, ((const uint16_t *)(const void *)frame)[0]);
    TEST_ASSERT_EQUAL_HEX16(0x3C00,
                            ((const uint16_t *)(const void *)(frame + layout.offsets[1]))[0]);
    TEST_ASSERT_TRUE(hdrg_format_layout(HDRG_FORMAT_RGBF32_PLANAR, 4, 1, true, &layout));
    hdrg_format_view(&layout, frame, &view);
    hdrg_format_pack(&exec, &layout, &yuv, codes, sizeof(codes), 0, 1, &view, NULL);
    TEST_ASSERT_TRUE(((const float *)(const void *)frame)[0] == 0.0f &&
                     ((const float *)(const void *)(frame + layout.offsets[1]))[0] == 1.0f &&
                     ((const float *)(const void *)(frame + layout.offsets[2]))[0] ==
                         32768.0f / 65535.0f);
    ((float *)(void *)frame)[1] = -0.5f;
    ((float *)(void *)(frame + layout.offsets[1]))[1] = 1.5f;
    hdrg_format_unpack(&exec, &layout, &yuv, &view, 0, 1, back, sizeof(back), NULL);
    TEST_ASSERT_TRUE(back[3] == 0u && back[4] == 65535u);

    /* 4:2:0 of a flat color */
    flat = (uint16_t *)malloc((size_t)16 * 8 * 3 * sizeof(uint16_t));
    TEST_ASSERT_NOT_NULL(flat);

    for (i = 0; i < 16u * 8u; i++) {
        flat[i * 3u] = 257u * 200u;
        flat[i * 3u + 1u] = 257u * 100u;
        flat[i * 3u + 2u] = 257u * 50u;
    }

    TEST_ASSERT_TRUE(hdrg_format_layout(HDRG_FORMAT_P016, 16, 8, true, &layout));
    scratch = malloc(hdrg_format_scratch(&layout));
    TEST_ASSERT_NOT_NULL(scratch);
    hdrg_format_view(&layout, frame, &view);
    hdrg_format_pack(&exec, &layout, &yuv, flat, 16u * 6u, 0, 8, &view, scratch);
    memset(back, 0, sizeof(back));
    hdrg_format_unpack(&exec, &layout, &yuv, &view, 0, 8, flat, 16u * 6u, scratch);

    for (i = 0; i < 16u * 8u; i++) {
        TEST_ASSERT_TRUE(abs((int32_t)flat[i * 3u] - 257 * 200) <= 32 &&
                         abs((int32_t)flat[i * 3u + 1u] - 257 * 100) <= 32 &&
                         abs((int32_t)flat[i * 3u + 2u] - 257 * 50) <= 32);
    }

    free(scratch);
    free(flat);
    hdrg_context_destroy(ctx);

    /* stills: a filter copying in each format, against the frame converted there and back */
    frames = (uint8_t *)malloc(bytes);
    expect = (uint16_t *)malloc(bytes * sizeof(uint16_t));
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && expect != NULL && rows != NULL && ref_rows != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
    }

    for (f = HDRG_FORMAT_RGB16; f <= HDRG_FORMAT_RGBF32_PLANAR; f++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_test_context(&ctx, NULL, 0));
        hdrg_context_exec(ctx, &exec);
        TEST_ASSERT_TRUE(hdrg_format_layout(f, TP_W, TP_H, true, &layout));
        scratch = malloc(hdrg_format_scratch(&layout) + 1u);
        output = (uint8_t *)malloc(layout.bytes);
        TEST_ASSERT_TRUE(scratch != NULL && output != NULL);

        for (i = 0; i < samples; i++) {
            expect[i] = (uint16_t)(frames[i] * 257u);
        }

        hdrg_format_view(&layout, output, &view);

        hdrg_format_pack(&exec, &layout, &yuv, expect, (size_t)TP_W * 6u, 0, TP_H, &view, scratch);
        hdrg_format_unpack(&exec, &layout, &yuv, &view, 0, TP_H, expect, (size_t)TP_W * 6u,
                           scratch);
        free(output);
        free(scratch);
        hdrg_context_destroy(ctx);

        ctx = format_context();
        window_add(ctx, "org.test.window.bands", "0", "0");
        memset(&raw, 0, sizeof(raw));
        raw.pixels = (const uint8_t *)(const void *)expect;
        raw.primaries = HDRG_PRIMARIES_BT709;
        raw.transfer = HDRG_TRANSFER_SRGB;
        raw.width = TP_W;
        raw.height = TP_H;
        raw.bit_depth = 16;
        raw.stride = (size_t)TP_W * 6u;
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_convert_raw(ctx, &raw, NULL, NULL, NULL, &ref, &ref_size));
        hdrg_context_destroy(ctx);

        for (i = 0; i < 2u; i++) {
            ctx = format_context();
            format_add(ctx, (i == 0) ? "org.test.format.frame" : "org.test.format.bands", f, 0, 0);

            if (i == 1u && f == HDRG_FORMAT_P016) {
                TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, async_still(ctx, frames, &output, &size));

                continue;
            }

            TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &output, &size));
            TEST_ASSERT_EQUAL_size_t(ref_size, size);
            TEST_ASSERT_EQUAL_MEMORY(ref, output, size);
            hdrg_free(output);
        }

        hdrg_free(ref);
    }

    /* a time window over RGBA16 frames (whole) and planar ones (bands) */
    hdrg_session_config_init(&config);
    ctx = window_context();
    window_add(ctx, "org.test.window.bands", "2", "0");
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows, &latency, &preroll));
    hdrg_context_destroy(ctx);

    for (i = 0; i < 2u; i++) {
        ctx = format_context();
        format_add(ctx, (i == 0) ? "org.test.format.frame" : "org.test.format.bands",
                   (i == 0) ? HDRG_FORMAT_RGBA16 : HDRG_FORMAT_RGB16_PLANAR, 2, 0);
        TEST_ASSERT_EQUAL_UINT32(
            TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
        hdrg_context_destroy(ctx);
        TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    }

    free(frame);
    free(frames);
    free(expect);
    free(rows);
    free(ref_rows);
}

/*
 * A filter's own frames (frame_alloc): the jobs' frames are its (the
 * input, a window's past frames, the output: their planes and strides, not
 * the host's alignment), every frame it made is freed, and a still or a
 * session's time window gives what the host's frames give. A frame not made,
 * strides short of a row and planes or strides off the samples' alignment
 * (the host reads the samples in place) fail the conversion (BANDS refuses
 * them at registration: test_malformed_reports_rejected). A create failing
 * unsupported fails it so (a GPU filter without a device), any other way as
 * the plugin's failure
 */
static void test_own_frames(void)
{
    static const uint32_t formats[3] = {HDRG_FORMAT_RGB16, HDRG_FORMAT_RGBA16, HDRG_FORMAT_P016};
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    hdrg_session_config_t config;
    hdrg_context_t *ctx;
    uint32_t i, f, latency, preroll;
    uint8_t *frames, *ref, *output, *rows, *ref_rows;
    size_t ref_size, size;

    frames = (uint8_t *)malloc(bytes);
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL && ref_rows != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
    }

    for (f = 0; f < 3u; f++) {
        ctx = format_context();
        format_add(ctx, "org.test.format.frame", formats[f], 0, 0);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &ref, &ref_size));

        memset(&g_frames, 0, sizeof(g_frames));
        ctx = format_context();
        format_add(ctx, "org.test.format.own", formats[f], 0, 3);
        TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &output, &size));
        TEST_ASSERT_EQUAL_size_t(ref_size, size);
        TEST_ASSERT_EQUAL_MEMORY(ref, output, size);
        TEST_ASSERT_EQUAL_UINT32(2, g_frames.made);
        TEST_ASSERT_EQUAL_UINT32(g_frames.made, g_frames.freed);
        TEST_ASSERT_EQUAL_UINT32(0, g_frames.mismatched);
        hdrg_free(ref);
        hdrg_free(output);
    }

    for (i = 1; i <= 5u; i += (i == 2u) ? 2u : 1u) {
        memset(&g_frames, 0, sizeof(g_frames));
        ctx = format_context();
        format_add(ctx, "org.test.format.own", HDRG_FORMAT_RGB16, 0, i);
        TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, async_still(ctx, frames, &output, &size));
        TEST_ASSERT_EQUAL_UINT32(g_frames.made, g_frames.freed);
    }

    /* a create failing: unsupported as it said (a GPU filter without a device), else the plugin's
     */
    for (i = 6; i <= 7u; i++) {
        ctx = format_context();
        format_add(ctx, "org.test.format.frame", HDRG_FORMAT_RGB16, 0, i);
        TEST_ASSERT_EQUAL_INT((i == 6u) ? HDRG_ERR_UNSUPPORTED : HDRG_ERR_PLUGIN,
                              async_still(ctx, frames, &output, &size));
    }

    /* a session's window over its own frames: the ring's 3 and the output */
    hdrg_session_config_init(&config);
    ctx = format_context();
    format_add(ctx, "org.test.format.frame", HDRG_FORMAT_RGBA16, 2, 0);
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows, &latency, &preroll));
    hdrg_context_destroy(ctx);

    memset(&g_frames, 0, sizeof(g_frames));
    ctx = format_context();
    format_add(ctx, "org.test.format.own", HDRG_FORMAT_RGBA16, 2, 3);
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    TEST_ASSERT_EQUAL_UINT32(4, g_frames.made);
    TEST_ASSERT_EQUAL_UINT32(4, g_frames.freed);
    TEST_ASSERT_EQUAL_UINT32(0, g_frames.mismatched);
    TEST_ASSERT_TRUE(g_frames.windows > 0);

    free(frames);
    free(rows);
    free(ref_rows);
}

/*
 * The jobs' frames: pts and HDRG_JOB_FIRST (a still's frame, a session's
 * first without a ring, a clip's first); a clip's overlap (windows past the
 * clip, the means, the delay and preroll); a SEQUENTIAL filter (its state
 * reset at clips) has every job run, a SCALE's preroll too (a seek gives
 * the whole run's frames), and needs a clip (and whole frames:
 * test_malformed_reports_rejected); cancel ends the jobs in flight of a
 * session destroyed with them
 */
static void test_job_contract(void)
{
    static const uint32_t seeks[3] = {2, 5, 7};
    static const uint8_t overlapped[TP_WFRAMES] = {0x03, 0x12, 0x21, 0x13, 0x22, 0x31, 0x11, 0x20};
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    hdrg_session_config_t config, fixed;
    hdrg_options_t *options;
    hdrg_session_frame_t in;
    hdrg_session_format_t raw;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    uint32_t i, latency, preroll, ref_latency, ref_preroll, start, got;
    uint16_t *expect;
    uint8_t *frames, *rows, *ref_rows, *seek, *output;
    size_t size;

    frames = (uint8_t *)malloc(bytes);
    expect = (uint16_t *)malloc(bytes * sizeof(uint16_t));
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    seek = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && expect != NULL && rows != NULL && ref_rows != NULL &&
                     seek != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
    }

    hdrg_session_config_init(&config);
    hdrg_session_config_init(&fixed);
    fixed.temporal = HDRG_TEMPORAL_STATIC;
    fixed.past = 0;
    fixed.flash_off = true;

    /* a still's frame and a session's, without a ring: the first, and the pts */
    ctx = window_context();
    window_add(ctx, "org.test.window.frame", "0", "0");
    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &output, &size));
    TEST_ASSERT_EQUAL_UINT32(HDRG_JOB_FIRST, g_windows.flags[0]);
    TEST_ASSERT_TRUE(g_windows.pts[0] == 0);
    hdrg_free(output);

    ctx = window_context();
    window_add(ctx, "org.test.window.frame", "0", "0");
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);

    for (i = 0; i < TP_WFRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT32((i == 0) ? HDRG_JOB_FIRST : 0u, g_windows.flags[i]);
        TEST_ASSERT_TRUE(g_windows.pts[i] == (int64_t)i);
    }

    /* a clip of 3 with an overlap of 1: its first frames, its windows */
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_PREFILTER, "org.test.window.frame", NULL, NULL, "3");
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);

    for (i = 0; i < TP_WFRAMES; i++) {
        TEST_ASSERT_EQUAL_UINT32((i % 3u == 0) ? HDRG_JOB_FIRST : 0u, g_windows.flags[i]);
    }

    ctx = window_context();
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_create(ctx, &options));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "clip", "3"));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_options_set(options, "overlap", "1"));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER,
                                                           "org.test.window.frame", options));
    hdrg_options_destroy(options);
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(overlapped, g_windows.seen, TP_WFRAMES);

    for (i = 0; i < bytes; i++) {
        expect[i] = (uint16_t)(frames[i] * 257u);
    }

    window_mean(expect, 1, 0, 3);
    ctx = window_context();
    window_add(ctx, "org.test.window.bands", "0", "0");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, config, (const uint8_t *)(const void *)expect, 16,
                                            0, 0, ref_rows, &ref_latency, &ref_preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    TEST_ASSERT_EQUAL_UINT32(ref_latency + 3u, latency);
    TEST_ASSERT_EQUAL_UINT32(ref_preroll + 3u, preroll);

    /* a SEQUENTIAL SCALE: every job runs, the preroll's too */
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.seq", NULL, NULL, "4");
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES,
                             window_session(ctx, fixed, frames, 8, 0, 0, rows, &latency, &preroll));
    TEST_ASSERT_EQUAL_UINT32(3, preroll);

    for (i = 0; i < 3u; i++) {
        start = seeks[i] - HDRG_MIN(seeks[i], preroll);
        got = window_session(ctx, fixed, frames, 8, start, seeks[i] - start, seek, &latency,
                             &ref_preroll);
        TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES - seeks[i], got);
        TEST_ASSERT_EQUAL_MEMORY(rows + (size_t)seeks[i] * samples, seek, (size_t)got * samples);
    }

    hdrg_context_destroy(ctx);

    memset(&raw, 0, sizeof(raw));
    raw.primaries = HDRG_PRIMARIES_BT709;
    raw.transfer = HDRG_TRANSFER_SRGB;
    raw.width = TP_W;
    raw.height = TP_H;
    raw.bit_depth = 8;
    raw.stride = (size_t)TP_W * 3u;
    ctx = window_context();
    window_add_at(ctx, HDRG_STAGE_SCALE, "org.test.window.seq", "0", "0", NULL);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN,
                          hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));
    hdrg_context_destroy(ctx);

    /* destroyed with jobs in flight that end only when canceled */
    if (TP_THREADS) {
        ctx = window_context();
        inflight_add(ctx, HDRG_STAGE_PREFILTER, "4", "3");
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_session_create(ctx, &raw, tp_plain(), NULL, &config, &session));
        memset(&in, 0, sizeof(in));
        in.strides[0] = raw.stride;

        for (i = 0; i < 2u; i++) {
            in.planes[0] = frames + (size_t)i * samples;
            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, &in));
        }

        hdrg_session_destroy(session);
        TEST_ASSERT_EQUAL_UINT32(1, g_async.cancels);
        hdrg_context_destroy(ctx);
    }

    free(frames);
    free(expect);
    free(rows);
    free(ref_rows);
    free(seek);
}

/* a context with the device, asynchronous and time window plugins, the chain of ids */
static hdrg_context_t *device_context(const char *const *ids, uint32_t count)
{
    hdrg_context_t *ctx;
    uint32_t i;

    ctx = window_context();
    memset(&g_devices, 0, sizeof(g_devices));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_register_plugin(ctx, device_init));

    for (i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_INT(HDRG_OK,
                              hdrg_context_add_filter(ctx, HDRG_STAGE_PREFILTER, ids[i], NULL));
    }

    return ctx;
}

/*
 * Frames on a device: neighboring working filters of one device kind keep
 * the frame there (the first takes the host's frame, the last gives one,
 * every frame between released once the next job ended), a filter between
 * of no device makes them host frames again, and the frames are what host
 * frames give, in a still and every frame of a session; the app's device
 * reaches the filters, and a filter on a device without device_release is
 * refused
 */
static void test_device_frames(void)
{
    const size_t samples = (size_t)TP_W * TP_H * 3u, bytes = samples * TP_WFRAMES;
    static const char *const three[3] = {"org.test.device.invert", "org.test.device.invert",
                                         "org.test.device.invert"},
                             *const broken[3] = {"org.test.device.invert", "org.test.async.cpu",
                                                 "org.test.device.invert"},
                             *const one[1] = {"org.test.async.cpu"},
                             *const two[2] = {"org.test.device.invert", "org.test.device.invert"},
                             *const lone[1] = {"org.test.device.norelease"};
    hdrg_session_config_t config;
    hdrg_context_t *ctx;
    uint32_t i, latency, preroll;
    uint8_t *frames, *ref, *output, *rows, *ref_rows, device, queue;
    size_t ref_size, size;

    frames = (uint8_t *)malloc(bytes);
    rows = (uint8_t *)malloc(bytes);
    ref_rows = (uint8_t *)malloc(bytes);
    TEST_ASSERT_TRUE(frames != NULL && rows != NULL && ref_rows != NULL);

    for (i = 0; i < bytes; i++) {
        frames[i] = (uint8_t)((i * 7u + i / samples * 40u) % 251u);
    }

    /* three linked: inverted three times, two frames on the device */
    ctx = device_context(one, 1);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &ref, &ref_size));
    ctx = device_context(three, 3);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG, hdrg_context_set_device(ctx, 0, &device, &queue));
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_INVALID_ARG,
                          hdrg_context_set_device(ctx, HDRG_DEVICE_KINDS + 1u, &device, &queue));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_set_device(ctx, HDRG_DEVICE_CUDA, &device, &queue));
    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &output, &size));
    TEST_ASSERT_EQUAL_size_t(ref_size, size);
    TEST_ASSERT_EQUAL_MEMORY(ref, output, size);
    TEST_ASSERT_TRUE(g_devices.device == &device && g_devices.queue == &queue);
    TEST_ASSERT_EQUAL_UINT32(1, g_devices.host_in);
    TEST_ASSERT_EQUAL_UINT32(1, g_devices.host_out);
    TEST_ASSERT_EQUAL_UINT32(2, g_devices.device_in);
    TEST_ASSERT_EQUAL_UINT32(2, g_devices.device_out);
    TEST_ASSERT_EQUAL_UINT32(2, g_devices.released);
    hdrg_free(output);

    /* a filter of no device between: host frames */
    ctx = device_context(broken, 3);
    TEST_ASSERT_EQUAL_INT(HDRG_OK, async_still(ctx, frames, &output, &size));
    TEST_ASSERT_EQUAL_MEMORY(ref, output, size);
    TEST_ASSERT_EQUAL_UINT32(2, g_devices.host_in);
    TEST_ASSERT_EQUAL_UINT32(2, g_devices.host_out);
    TEST_ASSERT_EQUAL_UINT32(0, g_devices.device_out);
    TEST_ASSERT_TRUE(g_devices.device == NULL);
    hdrg_free(output);
    hdrg_free(ref);

    /* a session: two linked (inverted twice) give the frames as they are */
    hdrg_session_config_init(&config);
    ctx = window_context();
    window_add(ctx, "org.test.window.bands", "0", "0");
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, ref_rows, &latency, &preroll));
    hdrg_context_destroy(ctx);

    ctx = device_context(two, 2);
    TEST_ASSERT_EQUAL_UINT32(
        TP_WFRAMES, window_session(ctx, config, frames, 8, 0, 0, rows, &latency, &preroll));
    hdrg_context_destroy(ctx);
    TEST_ASSERT_EQUAL_MEMORY(ref_rows, rows, bytes);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, g_devices.device_out);
    TEST_ASSERT_EQUAL_UINT32(TP_WFRAMES, g_devices.released);

    /* on a device without device_release */
    ctx = device_context(lone, 1);
    TEST_ASSERT_EQUAL_INT(HDRG_ERR_PLUGIN, async_still(ctx, frames, &output, &size));

    free(frames);
    free(rows);
    free(ref_rows);
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    int failures;

    g_alloc = hdrg_test_allocator();
    g_pixels = hdrg_test_image(TP_W, TP_H);

    UNITY_BEGIN();
    RUN_TEST(test_builtin_listed);
    RUN_TEST(test_malformed_reports_rejected);
    RUN_TEST(test_frozen_after_entry_point);
    RUN_TEST(test_unavailable_plugin);
    RUN_TEST(test_policy_rejects);
    RUN_TEST(test_options_validated);
    RUN_TEST(test_codec_users);
    RUN_TEST(test_gpu_shares);
    RUN_TEST(test_analyze_stage);
    RUN_TEST(test_stats_stage);
    RUN_TEST(test_dynamic_load);
    RUN_TEST(test_scale_stage_via_plugin);
    RUN_TEST(test_scaler_band_split_identical);
    RUN_TEST(test_polyphase_properties);
    RUN_TEST(test_denoise);
    RUN_TEST(test_deblock);
    RUN_TEST(test_dering);
    RUN_TEST(test_dering_gradient);
    RUN_TEST(test_defringe);
    RUN_TEST(test_deband);
    RUN_TEST(test_prefilter_chain);
    RUN_TEST(test_default_scaler_output_size);
    RUN_TEST(test_lifetime);
    RUN_TEST(test_async);
    RUN_TEST(test_async_thread);
    RUN_TEST(test_window);
    RUN_TEST(test_window_shots);
    RUN_TEST(test_inflight);
    RUN_TEST(test_clip_and_scale_window);
    RUN_TEST(test_latency_profiles);
    RUN_TEST(test_late);
    RUN_TEST(test_formats);
    RUN_TEST(test_own_frames);
    RUN_TEST(test_job_contract);
    RUN_TEST(test_device_frames);
    failures = UNITY_END();

    free(g_pixels);

    return failures;
}
