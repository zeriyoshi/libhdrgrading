/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

/*
 * libhdrgrading — the host side of plugins: registering plugins on a context
 * (an init function linked in, or a module loaded from an explicit path),
 * listing what they report, adding their filters to the stages a conversion
 * or a session runs, and choosing the codec a conversion writes. Plugins
 * themselves implement include/hdrgrading/filter.h (filters) and
 * include/hdrgrading/codec.h (codecs).
 */
#ifndef HDRGRADING_PLUGIN_H
#define HDRGRADING_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gpu.h"
#include "hdrgrading.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plugins implement include/hdrgrading/filter.h and codec.h and never link
 * the library; the host registers them on a context, adds filters by id to
 * the stages hdrg_convert and the sessions run, reads the files its codecs
 * recognize and writes the one chosen. Strings returned here are
 * validated copies, NUL terminated, valid until the context is destroyed.
 * Registration is not thread-safe: do it before converting.
 */

/* the calling convention across the plugin boundary (__cdecl on 32-bit x86 Windows) */
#if defined(_WIN32) && (defined(_M_IX86) || defined(__i386__))
#define HDRG_PLUGIN_CALL __cdecl
#else
#define HDRG_PLUGIN_CALL
#endif

/*
 * stages (insertion points): bits. PREFILTER, INTERP and SCALE run filters on
 * frames, ANALYZE judges a frame's lights (filter.h: hdrg_filter_analyze_fn_t),
 * STATS a video session's statistics across its frames (hdrg_stats_class_t)
 */
#define HDRG_STAGE_ANALYZE   UINT32_C(0x01)
#define HDRG_STAGE_PREFILTER UINT32_C(0x02)
#define HDRG_STAGE_INTERP    UINT32_C(0x04)
#define HDRG_STAGE_SCALE     UINT32_C(0x08)
#define HDRG_STAGE_STATS     UINT32_C(0x20)

#define HDRG_PREFILTER_MAX 6u /* filters of the PREFILTER stage */

/* filter properties: bits */
#define HDRG_FILTER_PURE          UINT32_C(0x01) /* jobs may run concurrently on one instance */
#define HDRG_FILTER_BANDS         UINT32_C(0x02) /* works on row bands (else whole frames) */
#define HDRG_FILTER_DETERMINISTIC UINT32_C(0x04) /* same output for any band split / threads */
#define HDRG_FILTER_ASYNC         UINT32_C(0x08) /* submits whole-frame jobs ended by job_done */
#define HDRG_FILTER_SEQUENTIAL                                                                     \
    UINT32_C(0x10) /* jobs keep state within a clip (filter_set_clip)                              \
                    */
/*
 * its device jobs (filter_set_device) take and give the frames on the device the library and
 * its plugins share (hdrg_gpu_share_frame_t of hdrgrading/foundation/gpu_share.h: native buffers
 * of the context's device, filter_gpu_share): the library may run a chain of such filters on its
 * own device frames (a session's frames kept on its GPU), the time windows too
 * (job_device_window)
 */
#define HDRG_FILTER_GPU_FRAMES UINT32_C(0x20)
/*
 * INTERP: its frames between take each pixel from the same place of the frames around them (a
 * repeat, a blend), moving nothing: a session does not cap their gain by the motion of the job
 * (a motion-compensated filter's frames between are capped where its motion is not sure)
 */
#define HDRG_FILTER_NO_MOTION UINT32_C(0x40)

/* plugin status and reasons */
/* DEGRADED works through a fallback (reason, status_message: which); UNAVAILABLE stays listed */
#define HDRG_PLUGIN_READY       UINT32_C(1)
#define HDRG_PLUGIN_DEGRADED    UINT32_C(2)
#define HDRG_PLUGIN_UNAVAILABLE UINT32_C(3)

#define HDRG_REASON_NONE               UINT32_C(0)
#define HDRG_REASON_NO_DEVICE          UINT32_C(1)
#define HDRG_REASON_HOST_TOO_OLD       UINT32_C(2) /* set by the host at load */
#define HDRG_REASON_PLUGIN_TOO_OLD     UINT32_C(3) /* set by the host at load */
#define HDRG_REASON_OS_UNSUPPORTED     UINT32_C(4)
#define HDRG_REASON_MISSING_RESOURCE   UINT32_C(5)
#define HDRG_REASON_LICENSE_RESTRICTED UINT32_C(6)
#define HDRG_REASON_CONFIGURATION      UINT32_C(7)
#define HDRG_REASON_OTHER              UINT32_C(8)
#define HDRG_REASON_REJECTED           UINT32_C(9) /* set by the host: invalid report or policy */

/* option types */
#define HDRG_OPTION_INT    UINT32_C(1)
#define HDRG_OPTION_FLOAT  UINT32_C(2)
#define HDRG_OPTION_STRING UINT32_C(3)

typedef struct hdrg_plugin hdrg_plugin_t;
typedef struct hdrg_plugin_api hdrg_plugin_api_t;
typedef struct hdrg_options hdrg_options_t;

/* the plugin's entry point (HDRG_PLUGIN_DEFINE, or passed directly when linked in) */
typedef hdrg_status_t(HDRG_PLUGIN_CALL *hdrg_plugin_init_fn_t)(hdrg_plugin_t *plugin,
                                                               const hdrg_plugin_api_t *api);

typedef struct {
    uint32_t struct_size; /* set to sizeof(hdrg_plugin_info_t) */
    const char *id, *name, *version, *vendor;
    const char *license; /* SPDX license expression */
    const char *copyright;
    const char *status_message; /* plugin's status text, or why it was rejected */
    uint32_t abi_version;       /* filter.h version the plugin was built with */
    uint32_t status;            /* HDRG_PLUGIN_READY / DEGRADED / UNAVAILABLE */
    uint32_t reason;            /* HDRG_REASON_* */
    uint32_t component_count, license_text_count, filter_count, codec_count;
    bool internal; /* built into the library */
    bool accepted; /* usable: validated, accepted by the policy, not UNAVAILABLE */
} hdrg_plugin_info_t;

typedef struct {
    uint32_t struct_size; /* set to sizeof(hdrg_component_info_t) */
    const char *name, *version, *license, *copyright, *notice;
} hdrg_component_info_t;

typedef struct {
    uint32_t struct_size; /* set to sizeof(hdrg_filter_info_t) */
    const char *id;
    uint32_t plugin_index;
    uint32_t stages; /* HDRG_STAGE_* bits */
    uint32_t flags;  /* HDRG_FILTER_* bits */
    uint32_t option_count;
} hdrg_filter_info_t;

/* a codec's roles (hdrg_codec_info_t.roles) */
#define HDRG_CODEC_READ  UINT32_C(0x01)
#define HDRG_CODEC_WRITE UINT32_C(0x02)

typedef struct {
    uint32_t struct_size; /* set to sizeof(hdrg_codec_info_t) */
    const char *id;
    const char *extensions; /* "jpg,jpeg": the first one is a written file's */
    uint32_t plugin_index;
    uint32_t roles;        /* HDRG_CODEC_READ / WRITE bits */
    uint32_t forms;        /* a writer's: bits 1 << HDRG_OUTPUT_* */
    uint32_t default_form; /* a writer's HDRG_OUTPUT_AUTO */
    uint32_t option_count; /* a writer's options */
} hdrg_codec_info_t;

typedef struct {
    uint32_t struct_size; /* set to sizeof(hdrg_option_info_t) */
    const char *key, *description;
    const char *default_text; /* STRING, else NULL */
    uint32_t type;            /* HDRG_OPTION_INT / FLOAT / STRING */
    int64_t default_int, min_int, max_int;
    double default_float, min_float, max_float;
} hdrg_option_info_t;

/*
 * Called once per external plugin after its entry point returned and the
 * report validated, before it is frozen; it may inspect the plugin through
 * the hdrg_context_plugin_* / filter_* functions. HDRG_OK accepts.
 */
typedef hdrg_status_t (*hdrg_plugin_policy_fn_t)(const hdrg_context_t *ctx, uint32_t plugin_index,
                                                 void *user);

HDRG_API hdrg_status_t hdrg_context_set_plugin_policy(hdrg_context_t *ctx,
                                                      hdrg_plugin_policy_fn_t fn, void *user);
/* the plugin stays listed (with the reason) even when rejected */
HDRG_API hdrg_status_t hdrg_context_register_plugin(hdrg_context_t *ctx,
                                                    hdrg_plugin_init_fn_t init);
/* path is UTF-8; nothing is searched. HDRG_ERR_UNSUPPORTED without dynamic loading */
HDRG_API hdrg_status_t hdrg_context_load_plugin(hdrg_context_t *ctx, const char *path);
/* detail of the last failed plugin / filter / option call, "" if none */
HDRG_API const char *hdrg_context_error(const hdrg_context_t *ctx);

HDRG_API uint32_t hdrg_context_plugin_count(const hdrg_context_t *ctx);
HDRG_API hdrg_status_t hdrg_context_plugin_info(const hdrg_context_t *ctx, uint32_t index,
                                                hdrg_plugin_info_t *out);
HDRG_API hdrg_status_t hdrg_context_plugin_component(const hdrg_context_t *ctx,
                                                     uint32_t plugin_index, uint32_t index,
                                                     hdrg_component_info_t *out);
HDRG_API hdrg_status_t hdrg_context_plugin_license_text(const hdrg_context_t *ctx,
                                                        uint32_t plugin_index, uint32_t index,
                                                        const char **spdx_id, const char **text);
HDRG_API uint32_t hdrg_context_filter_count(const hdrg_context_t *ctx);
HDRG_API hdrg_status_t hdrg_context_filter_info(const hdrg_context_t *ctx, uint32_t index,
                                                hdrg_filter_info_t *out);
HDRG_API hdrg_status_t hdrg_context_filter_option(const hdrg_context_t *ctx, uint32_t filter_index,
                                                  uint32_t option_index, hdrg_option_info_t *out);
/* the codecs of the context's usable plugins, the built-in ones first */
HDRG_API uint32_t hdrg_context_codec_count(const hdrg_context_t *ctx);
HDRG_API hdrg_status_t hdrg_context_codec_info(const hdrg_context_t *ctx, uint32_t index,
                                               hdrg_codec_info_t *out);
HDRG_API hdrg_status_t hdrg_context_codec_option(const hdrg_context_t *ctx, uint32_t codec_index,
                                                 uint32_t option_index, hdrg_option_info_t *out);

/*
 * Filter options as key=value text, as a CLI passes them: hdrg_context_add_filter
 * parses each value with the type and range the filter declared and rejects
 * unknown keys. Keys are [a-z0-9_] starting with [a-z], at most 64 bytes; at
 * most 128 per set.
 */
HDRG_API hdrg_status_t hdrg_options_create(hdrg_context_t *ctx, hdrg_options_t **out);
HDRG_API hdrg_status_t hdrg_options_set(hdrg_options_t *options, const char *key,
                                        const char *value);
HDRG_API void hdrg_options_destroy(hdrg_options_t *options);

/*
 * Adds a filter of a usable plugin to a stage of the following conversions:
 * HDRG_STAGE_SCALE takes one, HDRG_STAGE_PREFILTER up to HDRG_PREFILTER_MAX
 * that run in the order added, each on the previous one's output, all with
 * the side data of the frame as it came. PREFILTER filters run whatever the
 * params and replace the built-in ones (deblock, dering, defringe, deband,
 * MCTF, the noise reduction). HDRG_STAGE_INTERP takes one: the frames a video
 * session synthesizes between two input frames at another output rate
 * (hdrg_session_config_t.out_rate_num; default hdrg.interp.mc where the
 * build has it). HDRG_STAGE_ANALYZE takes one: the check of the judged lights
 * (hdrg_params_t.light_predictor > 0; default hdrg.analyze.predict).
 * HDRG_STAGE_STATS takes one: a video session's statistics across frames
 * (its temporal mode but STATIC and a WINDOWED without windows; default
 * hdrg.stats.temporal where the build has it). options may be NULL
 * (defaults) and is copied.
 */
HDRG_API hdrg_status_t hdrg_context_add_filter(hdrg_context_t *ctx, uint32_t stage, const char *id,
                                               const hdrg_options_t *options);
HDRG_API void hdrg_context_clear_filters(hdrg_context_t *ctx);
/*
 * The codec the following conversions write (hdrg_convert, hdrg_convert_raw):
 * a writer of a usable plugin by id (NULL: hdrg.jpeg, the default) with its
 * options (NULL: the defaults; parsed as hdrg_context_add_filter does,
 * copied). HDRG_ERR_NOT_FOUND for an id no usable writer has
 */
HDRG_API hdrg_status_t hdrg_context_set_writer(hdrg_context_t *ctx, const char *id,
                                               const hdrg_options_t *options);

#ifdef __cplusplus
}
#endif

#endif /* HDRGRADING_PLUGIN_H */
