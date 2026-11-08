/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <VapourSynth4.h>

#include "../test_util.h"
#include "hdrgrading/video.h"

/*
 * The VapourSynth plugin under a host that implements the
 * parts of the API it calls: the module is loaded as VapourSynth loads it,
 * hdrg.Grade is created through its registered function, and its frames are
 * requested in the ways fmUnordered allows (one call at a time, in any
 * order: in sequence, in batches whose calls are answered backwards or in a
 * scrambled order, at random). Every frame equals the session's output over
 * the whole clip, byte for byte, and carries the PQ color properties; no
 * call keeps a source frame past its return (their properties copied). With
 * fpsnum (twice the clip's rate, and fewer frames than the clip's), the
 * same requests give a session's frames at that output rate.
 */
#define TV_W        64
#define TV_H        48
#define TV_FRAMES   60
#define TV_KEYS     48u
#define TV_REQUESTS 512u
#define TV_BATCH    TV_FRAMES
#define TV_CLAMP(n) (((n) < 0) ? 0 : (((n) >= TV_FRAMES) ? TV_FRAMES - 1 : (n)))
#define TV_RATE     30
#define TV_OUT_MAX  (2 * TV_FRAMES) /* the reference's outputs at most */

/* the window (none in a build without the temporal filters) */
#if HDRG_BUILTIN_STATS
#define TV_PAST   4
#define TV_FUTURE 2
#else
#define TV_PAST   0
#define TV_FUTURE 0
#endif

struct VSMap {
    char keys[TV_KEYS][48];
    int types[TV_KEYS];
    int counts[TV_KEYS];
    int64_t ints[TV_KEYS];
    double floats[TV_KEYS][3];
    char data[TV_KEYS][160];
    int sizes[TV_KEYS];
    VSNode *nodes[TV_KEYS];
    uint32_t count;
    char error[256];
};

struct VSFrame {
    VSVideoFormat format;
    VSMap props;
    uint8_t *planes[3];
    ptrdiff_t strides[3];
    int width, height, refs;
    bool source; /* the source clip's (g_sources) */
};

struct VSNode {
    VSVideoInfo vi;
    VSFilterGetFrame get_frame;
    VSFilterFree free_fn;
    void *instance;
    bool source;
};

struct VSFrameContext {
    int requested[TV_REQUESTS];
    void *frame_data[4];
    uint32_t count, fresh;
    int n;
    bool failed;
    char error[256];
};

struct VSCore {
    int unused;
};

typedef struct {
    VSFrameContext ctx;
    const VSFrame *result;
} tv_job_t;

static VSPublicFunction g_grade;
static VSNode g_source, g_filter;
static VSCore g_core;
static const VSFrame *g_cache[TV_OUT_MAX];
static uint16_t *g_reference[TV_OUT_MAX][3];
static uint8_t g_plus[TV_OUT_MAX][HDRG_HDR10PLUS_T35_BYTES]; /* ... their T.35 payloads */
static uint8_t g_curve[TV_OUT_MAX][HDRG_ST2094_50_T35_MAX];
static size_t g_plus_size[TV_OUT_MAX], g_curve_size[TV_OUT_MAX];
static int g_out;     /* hdrg.Grade's fpsnum (0: none) */
static int g_outputs; /* the reference's frames */
static uint32_t g_cached;
static int g_linear;
static int g_sources; /* the source frames not freed yet */
static int g_maps;    /* the maps createMap made, not freed yet */
static void *g_module;
static char g_namespace[16];

static int tv_find(const VSMap *map, const char *key)
{
    uint32_t i;

    for (i = 0; i < map->count; i++) {
        if (strcmp(map->keys[i], key) == 0) {
            return (int)i;
        }
    }

    return -1;
}

/* the entry of key, made when it is missing */
static int tv_entry(VSMap *map, const char *key, int type)
{
    int i;

    i = tv_find(map, key);
    if (i >= 0) {
        map->types[i] = type;

        return i;
    }

    TEST_ASSERT_TRUE(map->count < TV_KEYS);
    i = (int)map->count++;
    strncpy(map->keys[i], key, sizeof(map->keys[i]) - 1u);
    map->types[i] = type;
    map->counts[i] = 0;

    return i;
}

static int64_t VS_CC tv_map_get_int(const VSMap *map, const char *key, int index, int *error)
{
    int i;

    i = tv_find(map, key);
    *error = (i < 0 || map->types[i] != ptInt || index != 0) ? peUnset : peSuccess;

    return (*error == peSuccess) ? map->ints[i] : 0;
}

static double VS_CC tv_map_get_float(const VSMap *map, const char *key, int index, int *error)
{
    int i;

    i = tv_find(map, key);
    *error = (i < 0 || map->types[i] != ptFloat || index != 0) ? peUnset : peSuccess;

    return (*error == peSuccess) ? map->floats[i][0] : 0.0;
}

static const char *VS_CC tv_map_get_data(const VSMap *map, const char *key, int index, int *error)
{
    int i;

    i = tv_find(map, key);
    *error = (i < 0 || map->types[i] != ptData || index != 0) ? peUnset : peSuccess;

    return (*error == peSuccess) ? map->data[i] : NULL;
}

static VSNode *VS_CC tv_map_get_node(const VSMap *map, const char *key, int index, int *error)
{
    int i;

    i = tv_find(map, key);
    *error = (i < 0 || map->types[i] != ptVideoNode || index != 0) ? peUnset : peSuccess;

    return (*error == peSuccess) ? map->nodes[i] : NULL;
}

static int VS_CC tv_map_num_elements(const VSMap *map, const char *key)
{
    int i;

    i = tv_find(map, key);

    return (i < 0) ? -1 : map->counts[i];
}

static int VS_CC tv_map_set_int(VSMap *map, const char *key, int64_t value, int append)
{
    int i;

    (void)append;

    i = tv_entry(map, key, ptInt);
    map->ints[i] = value;
    map->counts[i] = 1;

    return 0;
}

static int VS_CC tv_map_set_float(VSMap *map, const char *key, double value, int append)
{
    int i;

    (void)append;

    i = tv_entry(map, key, ptFloat);
    map->floats[i][0] = value;
    map->counts[i] = 1;

    return 0;
}

static int VS_CC tv_map_set_float_array(VSMap *map, const char *key, const double *values, int size)
{
    int i, k;

    i = tv_entry(map, key, ptFloat);
    for (k = 0; k < size && k < 3; k++) {
        map->floats[i][k] = values[k];
    }

    map->counts[i] = size;

    return 0;
}

static void tv_map_set_data(VSMap *map, const char *key, const char *value)
{
    int i;

    i = tv_entry(map, key, ptData);
    strncpy(map->data[i], value, sizeof(map->data[i]) - 1u);
    map->sizes[i] = (int)strlen(map->data[i]);
    map->counts[i] = 1;
}

static int VS_CC tv_map_set_bytes(VSMap *map, const char *key, const char *data, int size, int type,
                                  int append)
{
    int i;

    (void)type;
    (void)append;

    TEST_ASSERT_TRUE(size >= 0 && size <= (int)sizeof(map->data[0]));
    i = tv_entry(map, key, ptData);
    memcpy(map->data[i], data, (size_t)size);
    map->sizes[i] = size;
    map->counts[i] = 1;

    return 0;
}

static int VS_CC tv_map_get_data_size(const VSMap *map, const char *key, int index, int *error)
{
    int i;

    i = tv_find(map, key);
    *error = (i < 0 || map->types[i] != ptData || index != 0) ? peUnset : peSuccess;

    return (*error == peSuccess) ? map->sizes[i] : -1;
}

static void VS_CC tv_map_set_error(VSMap *map, const char *message)
{
    strncpy(map->error, message, sizeof(map->error) - 1u);
}

static int VS_CC tv_map_delete_key(VSMap *map, const char *key)
{
    int i;

    i = tv_find(map, key);
    if (i >= 0) {
        map->counts[i] = 0;
        map->types[i] = ptUnset;
    }

    return i >= 0;
}

static void VS_CC tv_copy_map(const VSMap *src, VSMap *dst)
{
    *dst = *src;
}

static VSMap *VS_CC tv_create_map(void)
{
    VSMap *map;

    map = (VSMap *)calloc(1, sizeof(*map));
    TEST_ASSERT_NOT_NULL(map);
    g_maps++;

    return map;
}

static void VS_CC tv_free_map(VSMap *map)
{
    if (map != NULL) {
        g_maps--;
    }

    free(map);
}

static void VS_CC tv_clear_map(VSMap *map)
{
    memset(map, 0, sizeof(*map));
}

static int VS_CC tv_map_consume_node(VSMap *map, const char *key, VSNode *node, int append)
{
    int i;

    (void)append;

    i = tv_entry(map, key, ptVideoNode);
    map->nodes[i] = node;
    map->counts[i] = 1;

    return 0;
}

static const VSMap *VS_CC tv_props_ro(const VSFrame *frame)
{
    return &frame->props;
}

static VSMap *VS_CC tv_props_rw(VSFrame *frame)
{
    return &frame->props;
}

static int VS_CC tv_query_format(VSVideoFormat *format, int family, int sample, int bits, int sub_w,
                                 int sub_h, VSCore *core)
{
    (void)core;

    format->colorFamily = family;
    format->sampleType = sample;
    format->bitsPerSample = bits;
    format->bytesPerSample = (bits > 8) ? 2 : 1;
    format->subSamplingW = sub_w;
    format->subSamplingH = sub_h;
    format->numPlanes = 3;

    return 1;
}

/* a frame of format with rows padded past their width, so strides differ from it */
static VSFrame *VS_CC tv_new_frame(const VSVideoFormat *format, int width, int height,
                                   const VSFrame *prop_src, VSCore *core)
{
    VSFrame *frame;
    int p, w, h;

    (void)core;

    frame = (VSFrame *)calloc(1, sizeof(*frame));
    TEST_ASSERT_NOT_NULL(frame);
    frame->format = *format;
    frame->width = width;
    frame->height = height;
    frame->refs = 1;

    for (p = 0; p < 3; p++) {
        w = (p == 0) ? width : width >> format->subSamplingW;
        h = (p == 0) ? height : height >> format->subSamplingH;
        frame->strides[p] = (ptrdiff_t)(w * format->bytesPerSample + 32);
        frame->planes[p] = (uint8_t *)calloc((size_t)(frame->strides[p] * h), 1);
        TEST_ASSERT_NOT_NULL(frame->planes[p]);
    }

    if (prop_src != NULL) {
        frame->props = prop_src->props;
    }

    return frame;
}

static void VS_CC tv_free_frame(const VSFrame *frame)
{
    VSFrame *owned = (VSFrame *)(uintptr_t)frame;
    int p;

    if (owned == NULL || --owned->refs > 0) {
        return;
    }

    for (p = 0; p < 3; p++) {
        free(owned->planes[p]);
    }

    g_sources -= owned->source ? 1 : 0;
    free(owned);
}

static const VSFrame *VS_CC tv_add_frame_ref(const VSFrame *frame)
{
    ((VSFrame *)(uintptr_t)frame)->refs++;

    return frame;
}

static ptrdiff_t VS_CC tv_get_stride(const VSFrame *frame, int plane)
{
    return frame->strides[plane];
}

static const uint8_t *VS_CC tv_get_read_ptr(const VSFrame *frame, int plane)
{
    return frame->planes[plane];
}

static uint8_t *VS_CC tv_get_write_ptr(VSFrame *frame, int plane)
{
    return frame->planes[plane];
}

static const VSVideoFormat *VS_CC tv_frame_format(const VSFrame *frame)
{
    return &frame->format;
}

static const VSVideoInfo *VS_CC tv_video_info(VSNode *node)
{
    return &node->vi;
}

static void VS_CC tv_free_node(VSNode *node)
{
    (void)node;
}

/*
 * Source frame t, 4:2:0 8-bit Y'CbCr of BT.709 (limited range): a gradient
 * with a bright square moving across it, a cut to a darker picture at 20, a
 * fade-out from 32 and a cut back at 48
 */
static VSFrame *tv_source_frame(int t)
{
    VSVideoFormat format;
    VSFrame *frame;
    uint8_t *row;
    int x, y;
    float k;

    tv_query_format(&format, cfYUV, stInteger, 8, 1, 1, &g_core);
    frame = tv_new_frame(&format, TV_W, TV_H, NULL, &g_core);
    frame->source = true;
    g_sources++;
    k = (t >= 20 && t < 48) ? 0.4f : 1.0f;

    for (x = 32; x <= t && t < 48; x++) {
        k *= 0.9f;
    }

    for (y = 0; y < TV_H; y++) {
        row = frame->planes[0] + y * frame->strides[0];

        for (x = 0; x < TV_W; x++) {
            row[x] = (uint8_t)(16.0f + (float)((x * 3 + y * 2) % 160) * k);

            if (x >= (t * 2) % (TV_W - 10) && x < (t * 2) % (TV_W - 10) + 10 && y >= 10 && y < 20) {
                row[x] = (uint8_t)(16.0f + 219.0f * k);
            }
        }
    }

    for (y = 0; y < TV_H / 2; y++) {
        for (x = 0; x < TV_W / 2; x++) {
            frame->planes[1][y * frame->strides[1] + x] = (uint8_t)(112 + x % 32);
            frame->planes[2][y * frame->strides[2] + x] = (uint8_t)(116 + y % 24);
        }
    }

    tv_map_set_int(&frame->props, "_Matrix", 1, maReplace);
    tv_map_set_int(&frame->props, "_Transfer", 1, maReplace);
    tv_map_set_int(&frame->props, "_Primaries", 1, maReplace);
    tv_map_set_int(&frame->props, "_ColorRange", 1, maReplace);

    return frame;
}

static void VS_CC tv_request(int n, VSNode *node, VSFrameContext *ctx)
{
    uint32_t i;

    TEST_ASSERT_TRUE(node == &g_source);
    n = TV_CLAMP(n);

    for (i = 0; i < ctx->count; i++) {
        if (ctx->requested[i] == n) {
            return;
        }
    }

    TEST_ASSERT_TRUE(ctx->count < TV_REQUESTS);
    ctx->requested[ctx->count++] = n;
    ctx->fresh++;
}

/* a requested source frame: the plugin may read only those */
static const VSFrame *VS_CC tv_get_frame_filter(int n, VSNode *node, VSFrameContext *ctx)
{
    uint32_t i;

    TEST_ASSERT_TRUE(node == &g_source);
    n = TV_CLAMP(n);

    for (i = 0; i < ctx->count; i++) {
        if (ctx->requested[i] == n) {
            return tv_source_frame(n);
        }
    }

    TEST_FAIL_MESSAGE("a source frame read without its request");

    return NULL;
}

static void VS_CC tv_cache_frame(const VSFrame *frame, int n, VSFrameContext *ctx)
{
    (void)ctx;

    TEST_ASSERT_TRUE(g_linear > 0);
    TEST_ASSERT_TRUE(n >= 0 && n < g_filter.vi.numFrames);
    tv_free_frame(g_cache[n]);
    g_cache[n] = tv_add_frame_ref(frame);
    g_cached++;
}

static void VS_CC tv_set_filter_error(const char *message, VSFrameContext *ctx)
{
    ctx->failed = true;
    strncpy(ctx->error, message, sizeof(ctx->error) - 1u);
}

static VSNode *VS_CC tv_create_filter(const char *name, const VSVideoInfo *vi,
                                      VSFilterGetFrame get_frame, VSFilterFree free_fn, int mode,
                                      const VSFilterDependency *deps, int num_deps, void *instance,
                                      VSCore *core)
{
    (void)name;
    (void)core;

    TEST_ASSERT_EQUAL_INT(fmUnordered, mode);
    TEST_ASSERT_TRUE(num_deps == 1 && deps[0].source == &g_source);

    g_filter.vi = *vi;
    g_filter.get_frame = get_frame;
    g_filter.free_fn = free_fn;
    g_filter.instance = instance;

    return &g_filter;
}

static int VS_CC tv_set_linear(VSNode *node)
{
    TEST_ASSERT_TRUE(node == &g_filter);
    g_linear = 8;

    return g_linear;
}

static const VSAPI g_api = {
    .createVideoFilter2 = tv_create_filter,
    .setLinearFilter = tv_set_linear,
    .freeNode = tv_free_node,
    .getVideoInfo = tv_video_info,
    .newVideoFrame = tv_new_frame,
    .freeFrame = tv_free_frame,
    .addFrameRef = tv_add_frame_ref,
    .getFramePropertiesRO = tv_props_ro,
    .getFramePropertiesRW = tv_props_rw,
    .getStride = tv_get_stride,
    .getReadPtr = tv_get_read_ptr,
    .getWritePtr = tv_get_write_ptr,
    .getVideoFrameFormat = tv_frame_format,
    .queryVideoFormat = tv_query_format,
    .getFrameFilter = tv_get_frame_filter,
    .requestFrameFilter = tv_request,
    .cacheFrame = tv_cache_frame,
    .setFilterError = tv_set_filter_error,
    .copyMap = tv_copy_map,
    .createMap = tv_create_map,
    .freeMap = tv_free_map,
    .clearMap = tv_clear_map,
    .mapSetError = tv_map_set_error,
    .mapDeleteKey = tv_map_delete_key,
    .mapNumElements = tv_map_num_elements,
    .mapGetInt = tv_map_get_int,
    .mapSetInt = tv_map_set_int,
    .mapGetFloat = tv_map_get_float,
    .mapSetFloat = tv_map_set_float,
    .mapSetFloatArray = tv_map_set_float_array,
    .mapGetData = tv_map_get_data,
    .mapGetDataSize = tv_map_get_data_size,
    .mapSetData = tv_map_set_bytes,
    .mapGetNode = tv_map_get_node,
    .mapConsumeNode = tv_map_consume_node,
};

static int VS_CC tv_config_plugin(const char *identifier, const char *space, const char *name,
                                  int version, int api, int flags, VSPlugin *plugin)
{
    (void)identifier;
    (void)name;
    (void)version;
    (void)api;
    (void)flags;
    (void)plugin;

    strncpy(g_namespace, space, sizeof(g_namespace) - 1u);

    return 1;
}

static int VS_CC tv_register_function(const char *name, const char *args, const char *type,
                                      VSPublicFunction fn, void *data, VSPlugin *plugin)
{
    (void)type;
    (void)data;
    (void)plugin;

    TEST_ASSERT_EQUAL_STRING("Grade", name);
    TEST_ASSERT_NOT_NULL(strstr(args, "clip:vnode;"));
    TEST_ASSERT_NOT_NULL(strstr(args, "diffuse_max:float:opt;"));
    TEST_ASSERT_NOT_NULL(strstr(args, "apl_peak_fraction:float:opt;"));
    TEST_ASSERT_NULL(strstr(args, "quality:"));
    g_grade = fn;

    return 1;
}

/* the session's output over the whole clip, with the arguments the tests give the plugin */
static void tv_reference(void)
{
    const VSFrame *src;
    hdrg_session_config_t config;
    hdrg_display_caps_t caps;
    hdrg_session_format_t format;
    hdrg_session_frame_t in;
    hdrg_session_output_t out;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;
    uint8_t p;
    int sent, received;

    memset(&format, 0, sizeof(format));
    format.width = TV_W;
    format.height = TV_H;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_BT709;
    hdrg_display_caps_init(&caps);
    caps.gamut = HDRG_GAMUT_BT2020;
    hdrg_session_config_init(&config);
    config.latency = HDRG_LATENCY_OFFLINE; /* the plugin's: a frame server's clips are files */
    config.past = TV_PAST;
    config.future = TV_FUTURE;
    config.input.layout = HDRG_YUV_420;
    config.input.matrix = HDRG_MATRIX_BT709;
    config.input.bits = 8;
    config.output.layout = HDRG_YUV_420;
    config.output.matrix = HDRG_MATRIX_BT2020;
    config.output.bits = 10;
    config.rate_num = TV_RATE;
    config.rate_den = 1;
    config.out_rate_num = (uint32_t)g_out;
    config.out_rate_den = (g_out != 0) ? 1u : 0u;

    for (received = 0; received < (int)TV_OUT_MAX; received++) {
        for (p = 0; p < 3u; p++) {
            free(g_reference[received][p]);
            g_reference[received][p] = NULL;
        }
    }

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    TEST_ASSERT_EQUAL_INT(HDRG_OK,
                          hdrg_session_create(ctx, &format, NULL, &caps, &config, &session));

    sent = 0;
    received = 0;

    for (;;) {
        TEST_ASSERT_TRUE(received < (int)TV_OUT_MAX);
        memset(&out, 0, sizeof(out));

        for (p = 0; p < 3u; p++) {
            g_reference[received][p] = (uint16_t *)calloc((size_t)(TV_W * TV_H), 2);
            out.planes[p] = (uint8_t *)g_reference[received][p];
            out.strides[p] = (size_t)((p == 0) ? TV_W : TV_W / 2) * 2u;
        }

        status = hdrg_session_receive_frame(session, &out);
        if (status == HDRG_ERR_AGAIN) {
            for (p = 0; p < 3u; p++) {
                free(g_reference[received][p]);
            }

            memset(&in, 0, sizeof(in));
            src = (sent < TV_FRAMES) ? tv_source_frame(sent) : NULL;

            for (p = 0; src != NULL && p < 3u; p++) {
                in.planes[p] = src->planes[p];
                in.strides[p] = (size_t)src->strides[p];
            }

            in.pts = sent;

            TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_session_send_frame(session, src ? &in : NULL));
            tv_free_frame(src);
            sent++;

            continue;
        }

        if (status == HDRG_EOF) {
            for (p = 0; p < 3u; p++) {
                free(g_reference[received][p]);
                g_reference[received][p] = NULL;
            }

            break;
        }

        TEST_ASSERT_EQUAL_INT(HDRG_OK, status);
        g_plus_size[received] =
            hdrg_hdr10plus_t35(&out.hdr10plus, g_plus[received], sizeof(g_plus[received]));
        g_curve_size[received] =
            hdrg_st2094_50_t35(&out.st2094_50, g_curve[received], sizeof(g_curve[received]));
        received++;
    }

    g_outputs = received;

    hdrg_session_destroy(session);
    hdrg_context_destroy(ctx);
}

/* hdrg.Grade over the source clip with the arguments of the reference */
static void tv_grade(void)
{
    VSMap in, out;
    int error;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    tv_map_consume_node(&in, "clip", &g_source, maReplace);
    tv_map_set_int(&in, "past", TV_PAST, maReplace);
    tv_map_set_int(&in, "future", TV_FUTURE, maReplace);

    if (g_out != 0) {
        tv_map_set_int(&in, "fpsnum", g_out, maReplace);
    }

    memset(g_cache, 0, sizeof(g_cache));
    g_grade(&in, &out, NULL, &g_core, &g_api);
    TEST_ASSERT_EQUAL_STRING("", out.error);
    TEST_ASSERT_TRUE(tv_map_get_node(&out, "clip", 0, &error) == &g_filter);
    TEST_ASSERT_TRUE(g_filter.vi.format.colorFamily == cfYUV &&
                     g_filter.vi.format.bitsPerSample == 10 &&
                     g_filter.vi.format.subSamplingW == 1 && g_filter.vi.format.subSamplingH == 1);
    TEST_ASSERT_EQUAL_INT(g_outputs, g_filter.vi.numFrames);
    TEST_ASSERT_EQUAL_INT64((g_out != 0) ? g_out : TV_RATE, g_filter.vi.fpsNum);
}

static void tv_free_filter(void)
{
    int n;

    g_filter.free_fn(g_filter.instance, &g_core, &g_api);
    TEST_ASSERT_EQUAL_INT(0, g_maps);

    for (n = 0; n < (int)TV_OUT_MAX; n++) {
        tv_free_frame(g_cache[n]);
        g_cache[n] = NULL;
    }
}

/* props' binary key: want's size bytes (size 0: no key) */
static void tv_bytes(const VSMap *props, const char *key, const uint8_t *want, size_t size)
{
    const char *data;
    int error;

    data = tv_map_get_data(props, key, 0, &error);

    if (size == 0) {
        TEST_ASSERT_NULL(data);

        return;
    }

    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_INT((int)size, tv_map_get_data_size(props, key, 0, &error));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, data, size);
}

/* the plugin's frame n against the reference: the planes, the color properties, the T.35 */
static void tv_check(const VSFrame *frame, int n)
{
    const uint16_t *row;
    int p, x, y, w, h, error;

    TEST_ASSERT_NOT_NULL(frame);

    for (p = 0; p < 3; p++) {
        w = (p == 0) ? TV_W : TV_W / 2;
        h = (p == 0) ? TV_H : TV_H / 2;

        for (y = 0; y < h; y++) {
            row = (const uint16_t *)(const void *)(frame->planes[p] + y * frame->strides[p]);

            for (x = 0; x < w; x++) {
                if (row[x] != g_reference[n][p][y * w + x]) {
                    TEST_FAIL_MESSAGE("a frame differs from the session's");
                }
            }
        }
    }

    TEST_ASSERT_EQUAL_INT64(16, tv_map_get_int(&frame->props, "_Transfer", 0, &error));
    TEST_ASSERT_EQUAL_INT64(9, tv_map_get_int(&frame->props, "_Matrix", 0, &error));
    TEST_ASSERT_EQUAL_INT64(9, tv_map_get_int(&frame->props, "_Primaries", 0, &error));
    TEST_ASSERT_EQUAL_INT64(1, tv_map_get_int(&frame->props, "_ColorRange", 0, &error));
    TEST_ASSERT_TRUE(tv_map_get_float(&frame->props, "MasteringDisplayMaxLuminance", 0, &error) >
                     100.0);
    tv_bytes(&frame->props, "HDR10Plus", g_plus[n], g_plus_size[n]);
    tv_bytes(&frame->props, "HdrgST2094_50", g_curve[n], g_curve_size[n]);
}

/* a context's call: true once it returned its frame (else it made new requests) */
static bool tv_call(tv_job_t *job, int reason)
{
    job->ctx.fresh = 0;
    job->result = g_filter.get_frame(job->ctx.n, reason, g_filter.instance, job->ctx.frame_data,
                                     &job->ctx, &g_core, &g_api);

    TEST_ASSERT_FALSE_MESSAGE(job->ctx.failed, job->ctx.error);

    /* the source frames freed by the call that got them: the properties are kept, not the frames */
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_sources, "a source frame held past its call");

    if (job->result != NULL) {
        return true;
    }

    TEST_ASSERT_TRUE_MESSAGE(job->ctx.fresh > 0, "a call returned nothing and requested nothing");

    return false;
}

/*
 * Frames ns[0..count) as concurrent requests: each one's first call in
 * order, then the rest answered in the order of step (1: in order, -1:
 * backwards, else scrambled by it) until every one returned its frame. A
 * frame in the cache is served from it, as the core does
 */
static void tv_batch(const int *ns, int count, int step)
{
    static tv_job_t jobs[TV_BATCH];
    int i, k, left;
    bool done[TV_BATCH];

    TEST_ASSERT_TRUE(count <= (int)TV_BATCH);
    memset(jobs, 0, sizeof(jobs));
    left = 0;

    for (i = 0; i < count; i++) {
        done[i] = g_cache[ns[i]] != NULL;

        if (done[i]) {
            tv_check(g_cache[ns[i]], ns[i]);

            continue;
        }

        jobs[i].ctx.n = ns[i];
        done[i] = tv_call(&jobs[i], arInitial);
        left++;
    }

    while (left > 0) {
        for (i = 0; i < count; i++) {
            k = (step == 1) ? i : ((step == -1) ? count - 1 - i : (i * step + 7) % count);

            if (done[k]) {
                continue;
            }

            done[k] = tv_call(&jobs[k], arAllFramesReady);

            if (done[k]) {
                tv_check(jobs[k].result, ns[k]);
                tv_free_frame(jobs[k].result);
                left--;
            }
        }
    }
}

static void test_sequential(void)
{
    int n;

    tv_grade();

    for (n = 0; n < TV_FRAMES; n++) {
        tv_batch(&n, 1, 1);
    }

    tv_free_filter();
}

/* batches of four answered backwards: the session advances past the frames of the earlier calls */
static void test_batches_backwards(void)
{
    int n, ns[4];

    tv_grade();
    g_cached = 0;

    for (n = 0; n < TV_FRAMES; n += 4) {
        ns[0] = n;
        ns[1] = n + 1;
        ns[2] = n + 2;
        ns[3] = n + 3;
        tv_batch(ns, 4, -1);
    }

    TEST_ASSERT_TRUE(g_cached > 0);
    tv_free_filter();
}

/* every frame at once, answered in a scrambled order */
static void test_all_scrambled(void)
{
    int n, ns[TV_FRAMES];

    tv_grade();

    for (n = 0; n < TV_FRAMES; n++) {
        ns[n] = n;
    }

    tv_batch(ns, TV_FRAMES, 37);
    tv_free_filter();
}

/* seeks: forward past the session's reach, backwards, and next to it again */
static void test_random_access(void)
{
    const int ns[8] = {45, 10, 11, 59, 3, 30, 31, 12};
    int i;

    tv_grade();

    for (i = 0; i < 8; i++) {
        tv_batch(&ns[i], 1, 1);
    }

    tv_free_filter();
}

/* whether the build interpolates (an out_rate session: hdrg.interp.mc, HDRG_WITH_TEMPORAL) */
static bool tv_interpolates(void)
{
    hdrg_session_config_t config;
    hdrg_session_format_t format;
    hdrg_session_t *session;
    hdrg_context_t *ctx;
    hdrg_status_t status;

    memset(&format, 0, sizeof(format));
    format.width = 16;
    format.height = 16;
    format.primaries = HDRG_PRIMARIES_BT709;
    format.transfer = HDRG_TRANSFER_SRGB;
    format.bit_depth = 8;
    format.stride = 48;
    hdrg_session_config_init(&config);
    config.rate_num = 30;
    config.rate_den = 1;
    config.out_rate_num = 60;
    config.out_rate_den = 1;

    TEST_ASSERT_EQUAL_INT(HDRG_OK, hdrg_context_create(NULL, &ctx));
    status = hdrg_session_create(ctx, &format, NULL, NULL, &config, &session);

    if (status == HDRG_OK) {
        hdrg_session_destroy(session);
    }

    hdrg_context_destroy(ctx);
    TEST_ASSERT_TRUE(status == HDRG_OK || status == HDRG_ERR_UNSUPPORTED);

    return status == HDRG_OK;
}

/*
 * fpsnum twice the clip's rate (the frames between interpolated) and lower
 * (12: frames passed over): in order, a scrambled batch, at random
 */
static void test_out_rate(void)
{
    const int seeks[8] = {90, 20, 21, 118, 5, 61, 62, 24};
    int rates[2], i, k, n, ns[TV_FRAMES];

    if (!tv_interpolates()) {
        TEST_IGNORE_MESSAGE("built without hdrg.interp.mc (HDRG_WITH_TEMPORAL)");
    }

    rates[0] = 2 * TV_RATE;
    rates[1] = 12;

    for (k = 0; k < 2; k++) {
        g_out = rates[k];
        tv_reference();
        TEST_ASSERT_EQUAL_INT((k == 0) ? 2 * TV_FRAMES - 1 : (TV_FRAMES - 1) * 12 / TV_RATE + 1,
                              g_outputs);

        tv_grade();

        for (n = 0; n < g_outputs; n++) {
            tv_batch(&n, 1, 1);
        }

        tv_free_filter();
        tv_grade();

        for (n = 0; n < g_outputs && n < (int)TV_FRAMES; n++) {
            ns[n] = n;
        }

        tv_batch(ns, n, 37);
        tv_free_filter();
        tv_grade();

        for (i = 0; i < 8; i++) {
            n = seeks[i] % g_outputs;
            tv_batch(&n, 1, 1);
        }

        tv_free_filter();
    }

    g_out = 0;
    tv_reference();
}

/* unknown choices and out-of-range settings fail the call with a message */
static void test_bad_arguments(void)
{
    VSMap in, out;

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    tv_map_consume_node(&in, "clip", &g_source, maReplace);
    tv_map_set_data(&in, "preset", "cartoon");
    g_grade(&in, &out, NULL, &g_core, &g_api);
    TEST_ASSERT_NOT_NULL(strstr(out.error, "preset"));

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    tv_map_consume_node(&in, "clip", &g_source, maReplace);
    tv_map_set_float(&in, "strength", 7.0, maReplace);
    g_grade(&in, &out, NULL, &g_core, &g_api);
    TEST_ASSERT_NOT_NULL(strstr(out.error, "hdrg.Grade"));
}

void setUp(void)
{
}

void tearDown(void)
{
}

int main(void)
{
    VSInitPlugin init;
    VSPLUGINAPI plugin_api;
    uint8_t p;
    int failures, n;
    void *symbol;

    g_module = dlopen(HDRG_TEST_VS_PLUGIN, RTLD_NOW | RTLD_LOCAL);
    if (g_module == NULL) {
        return 1;
    }

    symbol = dlsym(g_module, "VapourSynthPluginInit2");
    if (symbol == NULL) {
        return 1;
    }

    /* POSIX: object and function pointers share one representation */
    memcpy(&init, &symbol, sizeof(init));

    memset(&plugin_api, 0, sizeof(plugin_api));
    plugin_api.configPlugin = tv_config_plugin;
    plugin_api.registerFunction = tv_register_function;

    memset(&g_source, 0, sizeof(g_source));
    tv_query_format(&g_source.vi.format, cfYUV, stInteger, 8, 1, 1, &g_core);
    g_source.vi.width = TV_W;
    g_source.vi.height = TV_H;
    g_source.vi.numFrames = TV_FRAMES;
    g_source.vi.fpsNum = TV_RATE;
    g_source.vi.fpsDen = 1;
    g_source.source = true;

    UNITY_BEGIN();
    init(NULL, &plugin_api);
    TEST_ASSERT_EQUAL_STRING("hdrg", g_namespace);
    tv_reference();
    RUN_TEST(test_sequential);
    RUN_TEST(test_batches_backwards);
    RUN_TEST(test_all_scrambled);
    RUN_TEST(test_random_access);
    RUN_TEST(test_out_rate);
    RUN_TEST(test_bad_arguments);
    failures = UNITY_END();

    for (n = 0; n < (int)TV_OUT_MAX; n++) {
        for (p = 0; p < 3u; p++) {
            free(g_reference[n][p]);
        }
    }

    dlclose(g_module);

    return failures;
}
