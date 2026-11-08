/*
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 * Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
 */

#ifndef HDRG_HOST_DEFAULTS_H
#define HDRG_HOST_DEFAULTS_H

#include <stdbool.h>
#include <stdint.h>

#include "host.h"

/* the built-in classes the library takes where the app named none: hdrg_defaults' */
#define HDRG_DEFAULT_DEBLOCK  0u /* PREFILTER: params' deblock_strength */
#define HDRG_DEFAULT_DERING   1u /* ... dering_strength */
#define HDRG_DEFAULT_DEFRINGE 2u /* ... defringe_strength */
#define HDRG_DEFAULT_DEBAND   3u /* ... deband_strength */
#define HDRG_DEFAULT_MCTF     4u /* ... a session's mctf_strength */
#define HDRG_DEFAULT_DENOISE  5u /* ... grain_policy REDUCE and RESYNTH */
#define HDRG_DEFAULT_LANCZOS  6u /* SCALE: params' upscaler, HDRG_UPSCALER_* after this */
#define HDRG_DEFAULT_EDGE     7u
#define HDRG_DEFAULT_FSR1     8u
#define HDRG_DEFAULT_NIS      9u
#define HDRG_DEFAULT_INTERP   10u /* INTERP: a session's output rate */
#define HDRG_DEFAULT_STATS    11u /* STATS: a session's statistics across frames */
#define HDRG_DEFAULT_PREDICT  12u /* ANALYZE: params' light_predictor */
#define HDRG_DEFAULT_WRITER   13u /* a still's writer (hdrg_context_set_writer's none) */
#define HDRG_DEFAULTS         14u

/* a default: its CPU class's id, its GPU versions' name after hdrg.metal. and so on (NULL: none) */
typedef struct {
    const char *id;
    const char *gpu;
} hdrg_default_t;

extern const hdrg_default_t hdrg_defaults[HDRG_DEFAULTS];

/* default d's CPU class (NULL: the build left it out) */
extern const hdrg_class_t *hdrg_default_find(const hdrg_plugin_host_t *host, uint8_t d);

/*
 * The next class to try for default d from *at (0 at first; advanced past
 * it): with gpu (the context's HDRG_GPU_AUTO / FILTERS) the GPU versions in
 * the build, of kind first (an HDRG_DEVICE_*, 0: none) then Metal's,
 * Vulkan's and WebGPU's, then the CPU's (*cpu true); NULL past it (or no
 * CPU class: the build left it out)
 */
extern const hdrg_class_t *hdrg_default_class(const hdrg_plugin_host_t *host, uint8_t d, bool gpu,
                                              uint32_t kind, uint8_t *at, bool *cpu);

#endif /* HDRG_HOST_DEFAULTS_H */
