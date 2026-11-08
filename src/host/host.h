/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_HOST_H
#define HDRG_HOST_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdrgrading/foundation/thread.h"
#include "../core/exec.h"
#include "format.h"
#include "hdrgrading/filter.h"

/* boundary limits */
#define HDRG_LIMIT_ID         128u
#define HDRG_LIMIT_NAME       256u
#define HDRG_LIMIT_VERSION    64u
#define HDRG_LIMIT_LICENSE    512u
#define HDRG_LIMIT_COPYRIGHT  1024u
#define HDRG_LIMIT_TEXT       65536u  /* one license text or NOTICE */
#define HDRG_LIMIT_TEXT_TOTAL 262144u /* all of a plugin's texts and NOTICEs */
#define HDRG_LIMIT_COMPONENTS 64u
#define HDRG_LIMIT_MESSAGE    1024u
#define HDRG_LIMIT_KEY        64u
#define HDRG_LIMIT_OPTIONS    128u
#define HDRG_LIMIT_CLASSES    64u
#define HDRG_LIMIT_ERROR      256u
#define HDRG_LIMIT_INFLIGHT   8u  /* frames an ASYNC filter may have in flight */
#define HDRG_LIMIT_WINDOW     8u  /* PREFILTER: frames before / after its own a job may read */
#define HDRG_LIMIT_WINDOW_MAX 32u /* ... SCALE (low resolution frames: longer clips) */
#define HDRG_LIMIT_INTERP     4u  /* INTERP: frames before t and after t + 1 */

/* a validated, NUL-terminated copy owned by the host */
typedef struct {
    char *data;
    size_t size;
} hdrg_text_t;

typedef struct {
    hdrg_text_t key, description, def_text; /* def_text: STRING */
    uint32_t type;
    int64_t def_int, min_int, max_int;
    double def_float, min_float, max_float;
} hdrg_option_spec_t;

typedef struct hdrg_plugin_host hdrg_plugin_host_t;

struct hdrg_class {
    uint32_t magic;
    hdrg_plugin_t *plugin;
    hdrg_text_t id;
    hdrg_option_spec_t *options;
    hdrg_filter_create_fn_t create;
    hdrg_filter_destroy_fn_t destroy;
    hdrg_filter_configure_fn_t configure;
    hdrg_filter_process_fn_t process;
    hdrg_filter_submit_fn_t submit;
    hdrg_filter_analyze_fn_t analyze;
    hdrg_stats_window_fn_t stats_window; /* a STATS class's work (class_set_stats) */
    hdrg_stats_fade_fn_t stats_fade;
    hdrg_stats_frame_fn_t stats_frame;
    hdrg_stats_render_fn_t stats_render;
    hdrg_stats_fit_fn_t stats_fit;
    hdrg_stats_rendered_fn_t stats_rendered;
    hdrg_filter_input_rows_fn_t input_rows;
    hdrg_filter_frame_alloc_fn_t frame_alloc;
    hdrg_filter_frame_free_fn_t frame_free;
    hdrg_filter_cancel_fn_t cancel;
    hdrg_filter_device_release_fn_t device_release;
    /* a codec's (codec.h) */
    hdrg_text_t extensions;
    hdrg_codec_probe_fn_t probe;
    hdrg_codec_read_fn_t read;
    hdrg_codec_create_fn_t write_create;
    hdrg_codec_rows_fn_t write_rows;
    hdrg_codec_finish_fn_t write_finish;
    hdrg_codec_destroy_fn_t write_destroy;
    uint32_t forms, default_form, rows, color, max_side;
    void *user; /* class_set_user */
    uint32_t stages, flags, option_count;
    uint8_t kind; /* HDRG_CLASS_*: what its setters made it (0: neither yet) */
    bool registered;
};

/* hdrg_class_t.kind */
#define HDRG_CLASS_FILTER UINT8_C(1)
#define HDRG_CLASS_CODEC  UINT8_C(2)

typedef struct {
    hdrg_text_t name, version, license, copyright, notice;
} hdrg_component_t;

typedef struct {
    hdrg_text_t id, text;
} hdrg_license_text_t;

struct hdrg_plugin {
    uint32_t magic;
    hdrg_plugin_host_t *host;
    void *module; /* loaded module; NULL when linked in */
    hdrg_text_t id, name, version, vendor, license, copyright, message;
    hdrg_component_t *components;
    hdrg_license_text_t *texts;
    hdrg_class_t **classes;
    hdrg_plugin_deinit_fn_t deinit;
    void *user; /* plugin_set_user */
    size_t text_bytes;
    uint32_t abi_version, status, reason, component_count, text_count, class_count;
    bool internal, frozen, accepted, has_info, has_license, has_status;
};

/* an option set: host text values, or every declared key resolved to its type */
typedef struct {
    hdrg_text_t key, value; /* value: the text given, and a resolved STRING's */
    uint32_t type;          /* 0 = text, else HDRG_OPTION_INT / FLOAT / STRING */
    int64_t int_value;
    double float_value;
} hdrg_option_t;

struct hdrg_options {
    uint32_t magic;
    hdrg_plugin_host_t *host;
    hdrg_option_t *items;
    uint32_t count;
    bool resolved;
};

typedef struct {
    const hdrg_class_t *cls;
    hdrg_options_t *options; /* resolved */
    uint32_t stage;
} hdrg_filter_entry_t;

/*
 * the context's own GPU devices of the kinds the app gave none of (hdrg_host_device), and the
 * library's objects around the app's devices the plugins share (filter_gpu_share)
 */
typedef struct {
    void *object;
    uint32_t kind;
} hdrg_host_retired_t;

typedef struct {
    hdrg_mutex_t lock, share_lock;
    void *objects[HDRG_DEVICE_KINDS]; /* hdrg_metal_t *, hdrg_vulkan_t *, hdrg_gpu_t * (lock) */
    void *wrapped[HDRG_DEVICE_KINDS]; /* ... around the app's device (share_lock; NULL: none) */
    void *wrapped_of[HDRG_DEVICE_KINDS][2]; /* ... of this device and queue (share_lock) */
    hdrg_host_retired_t *retired; /* the wraps of the devices the app replaced, kept to the end
                                     (the filters made on them hold them; share_lock) */
    uint32_t retired_count;
    bool tried[HDRG_DEVICE_KINDS];      /* lock */
    bool wrap_tried[HDRG_DEVICE_KINDS]; /* share_lock */
} hdrg_host_own_t;

/* api first: a table the plugins received is its host (the memory functions find the allocator) */
struct hdrg_plugin_host {
    hdrg_plugin_api_t api;
    hdrg_allocator_t alloc;
    const hdrg_context_t *ctx; /* passed to the policy */
    hdrg_plugin_t **plugins;
    hdrg_filter_entry_t *filters;
    hdrg_plugin_policy_fn_t policy;
    void *policy_user;
    void *devices[HDRG_DEVICE_KINDS][2]; /* the app's device and queue of each kind */
    hdrg_host_own_t *own;                /* the context's own devices (NULL: none to be made) */
    const hdrg_class_t *writer;          /* hdrg_context_set_writer's (NULL: hdrg.jpeg) */
    hdrg_options_t *writer_options;      /* ... resolved (NULL: the defaults) */
    hdrg_exec_t exec;                    /* the codecs' threads: the running conversion's */
    uint32_t plugin_count, filter_count;
    char error[HDRG_LIMIT_ERROR];
};

/* handles the host gives a running filter */
struct hdrg_filter {
    uint32_t magic;
    const hdrg_class_t *cls;
    const hdrg_plugin_host_t *host;    /* its devices */
    const hdrg_stats_session_t *stats; /* a STATS filter's session (NULL: none) */
    uint32_t device;                   /* filter_set_device (0: none) */
    uint32_t stage;                    /* the stage it runs in (its window's limit) */
    uint32_t past, future;             /* filter_set_window, or a clip's */
    uint32_t clip, overlap;            /* filter_set_clip (clip 0: none) */
    uint32_t in_format;                /* filter_set_input */
    hdrg_format_yuv_t yuv;             /* ... P016's matrix and range */
    uint32_t max_inflight;             /* filter_set_max_inflight (0: not set) */
    bool creating;
};

struct hdrg_negotiation {
    uint32_t magic;
    const float *noise;    /* HDRG_SD_NOISE v1 of the input frame; NULL = none */
    const uint8_t *motion; /* HDRG_SD_MOTION v1 of the input frame (checked); NULL = none */
    size_t motion_size;
    const double *xy; /* the input's primaries and white (nego_input_color); NULL = none */
    uint32_t transfer, bits;
    uint32_t in_width, in_height, in_format, req_width, req_height;
    uint32_t out_width, out_height, out_format, halo;
    bool output_set, passthrough;
};

/*
 * A job's output side data (job_set_side_data: INTERP's HDRG_SD_MOTION),
 * its stage's, grown with alloc; size 0: none
 */
typedef struct {
    const hdrg_allocator_t *alloc;
    hdrg_mutex_t lock; /* a submitted job sets it from any thread */
    uint8_t *data;
    size_t size, cap;
} hdrg_job_side_t;

/* the end of a submitted (ASYNC) job, signalled by job_done */
typedef struct {
    hdrg_mutex_t mutex;
    hdrg_cond_t cond;
    hdrg_status_t result;
    bool ended;
} hdrg_job_end_t;

/* a worker's scratch for the jobs it runs (job_scratch): kept and grown */
typedef struct {
    const hdrg_allocator_t *alloc;
    void *data;
    size_t bytes;
} hdrg_job_scratch_t;

struct hdrg_job {
    uint32_t magic;
    const uint8_t *in_planes[3]; /* each plane's row of input row in_begin */
    uint8_t *out_planes[3];      /* ... of output row begin */
    hdrg_job_end_t *end_of;      /* ASYNC: where job_done signals; NULL = a process job */
    const hdrg_format_frame_t *const *window; /* frame t + k's planes at window[k] (k from -past
                                                 to future, not 0) */
    void *const *window_handles;              /* ... the plugin's frames */
    void *in_handle, *out_handle;             /* the plugin's frames of a whole frame's job */
    void *in_device, *out_device;             /* frames on the device: the input, the output */
    void *const
        *device_window;    /* frame t + k on the device at device_window[k]; NULL: the host's */
    hdrg_job_side_t *side; /* the output's side data (INTERP); NULL: none */
    hdrg_job_scratch_t *scratch; /* the worker's scratch (job_scratch); NULL: none (submit) */
    size_t *asked;               /* a band's job: the most it asked for (the stage grows every
                                    worker's to the band's most before the next) */
    uint64_t in_ready, out_ready;
    size_t in_strides[3], out_strides[3];
    int64_t pts;
    uint32_t in_format, flags;
    uint32_t begin, end, in_begin, in_end, index, past, future;
    uint32_t phase;         /* INTERP: the output between the frame (0) and the next (65536) */
    const uint32_t *phases; /* INTERP: the outputs' phases between the two, ascending */
    uint32_t phase_count;
    uint32_t shot_past, shot_future; /* the window's frames in the frame's shot */
    bool shots;                      /* shot_past and shot_future known (not PREFILTER) */
    uint8_t in_planes_n, out_planes_n;
    uint8_t out_wanted; /* the output goes on the device */
};

#define HDRG_OPTIONS_MAGIC UINT32_C(0x484F5054)
#define HDRG_FILTER_MAGIC  UINT32_C(0x48464C54)
#define HDRG_NEGO_MAGIC    UINT32_C(0x48474E47)
#define HDRG_JOB_MAGIC     UINT32_C(0x484A4F42)

extern void hdrg_host_init(hdrg_plugin_host_t *host, const hdrg_allocator_t *alloc,
                           const hdrg_context_t *ctx);
/* unregisters every plugin (deinit, module unload) */
extern void hdrg_host_destroy(hdrg_plugin_host_t *host);
/*
 * The device and queue of kind for the context's filters (filter_device) and
 * its core: the app's (hdrg_context_set_device), else the context's own,
 * made on the first ask and shared by them all (Metal: the system's default
 * device and a queue; WebGPU: a high-performance adapter's device, used
 * from one thread: src/gpu/webgpu). false: none (no such GPU or backend)
 */
extern bool hdrg_host_device(const hdrg_plugin_host_t *host, uint32_t kind, void **device,
                             void **queue);

/*
 * the built-in plugins (src/plugins), in the order every context registers them (generated): the
 * filters', then the codecs' from hdrg_builtin_codecs_first
 */
extern const hdrg_plugin_init_fn_t hdrg_builtin_plugins[];
extern const uint32_t hdrg_builtin_plugin_count, hdrg_builtin_codecs_first;
/*
 * Collects, validates and freezes one plugin; module (may be NULL) is
 * unloaded here unless the plugin ends up usable. The plugin stays listed
 * whatever the outcome.
 */
extern hdrg_status_t hdrg_host_register(hdrg_plugin_host_t *host, hdrg_plugin_init_fn_t init,
                                        void *module, uint32_t abi_version, bool internal);

/* usable class by id; NULL = none */
extern const hdrg_class_t *hdrg_host_find_class(const hdrg_plugin_host_t *host, const char *id,
                                                size_t id_len);
/* the first filter added to a stage; NULL = none */
extern const hdrg_filter_entry_t *hdrg_host_stage_filter(const hdrg_plugin_host_t *host,
                                                         uint32_t stage);
/* the filters added to a stage */
extern uint32_t hdrg_host_stage_count(const hdrg_plugin_host_t *host, uint32_t stage);
/* text options (NULL = none) -> every declared key, typed and range-checked */
extern hdrg_status_t hdrg_host_resolve_options(hdrg_plugin_host_t *host, const hdrg_class_t *cls,
                                               const hdrg_options_t *text, hdrg_options_t **out);
/* detail must be validated text (an id or a key) */
extern void hdrg_host_set_error(hdrg_plugin_host_t *host, const char *message, const char *detail,
                                size_t detail_len);
/* the table a class's callbacks receive: its context's */
extern const hdrg_plugin_api_t *hdrg_class_api(const hdrg_class_t *cls);
/* a class declaring HDRG_FILTER_GPU_FRAMES: its device frames are the library's GPU layer's */
extern bool hdrg_class_library_gpu(const hdrg_class_t *cls);

/*
 * loader.c: opens a module and reads its two exports; the version function
 * is called, the entry point is not. HDRG_ERR_NOT_FOUND when it cannot be
 * opened, HDRG_ERR_UNSUPPORTED when it is not a plugin or the platform has
 * no loader.
 */
extern hdrg_status_t hdrg_module_open(const hdrg_allocator_t *alloc, const char *path,
                                      void **module, uint32_t *abi_version,
                                      hdrg_plugin_init_fn_t *init);
extern void hdrg_module_close(void *module);

#endif /* HDRG_HOST_HOST_H */
